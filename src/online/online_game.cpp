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
// - The pause menu is off.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompui/recompui.h"
#include "online.h"
#include "rush2.h"
#include "battle.h"

// ultramodern's conversion of a controller's stick to the N64 stick's range and shape (ultramodern/src/input.cpp).
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

extern "C" void physics_thread_step_once_80076D70(uint8_t* rdram, recomp_context* ctx);
extern "C" void race_end_to_menu_800AAC64(uint8_t* rdram, recomp_context* ctx);
extern "C" void menu_screen_dispatch_800AE0D4(uint8_t* rdram, recomp_context* ctx);
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
    constexpr int32_t record_profile = 0x24;         // The player's save profile (a controller's buffer, 0x6C0 bytes)
    // Online every player races with the same blank guest profile, so nobody's saved keys or unlocks change the race
    // (the track's keys spawn when not every racer has them, func_8005F00C). RDRAM 0x80F80000-0x81000000 is unused.
    constexpr uint32_t guest_profile = 0x80FF0000;
    constexpr uint32_t profile_size = 0x6C0;
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
        for (int slot = 0; slot < max_players; slot++) {
            if (!roster[slot].used) {
                continue;
            }
            if (slot == me) {
                local_player = race_players;
            }
            cars.push_back(roster[slot].car);
            race_players++;
        }
        race_players = std::clamp(race_players, 1, (int)max_players);

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
        MEM_B(0, (int32_t)track_id) = (int8_t)settings.track;
        for (int i = 0; i < (int)settings.menu.size(); i++) {
            MEM_B(i, (int32_t)menu_settings) = (int8_t)settings.menu[i];
        }

        for (uint32_t i = 0; i < profile_size; i++) {
            MEM_B(0, (int32_t)(guest_profile + i)) = 0;
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
            MEM_B(record_range, record) = default_range;
            MEM_B(record_range + 1, record) = default_range;
            MEM_W(record_profile, record) = (int32_t)guest_profile;
            int32_t entry = (int32_t)(car_table + p * car_entry_size);
            MEM_B(1, entry) = (int8_t)cars[p].type;
            for (int i = 0; i < 4; i++) {
                MEM_B(2 + i, entry) = (int8_t)cars[p].colors[i];
            }
        }
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
        for (int p = 0; p < race_players; p++) {
            add(player_records + p * player_record_size, player_record_size);
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

    // The race is over (game state 6): the machines part. The post-race works on the game's players 1 and up, so
    // this machine's player becomes the only one, with the record the menus gave them (their profile and controller)
    // and their car; it saves their stats and times and runs their name entry as after a local race.
    void finish_race_locally(uint8_t* rdram) {
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
                    if (r.address != car_table) {
                        MEM_B(0, (int32_t)(r.address + i)) = (int8_t)menu_snapshot[at];
                    }
                }
            }
            menu_snapshot.clear();
        }
        MEM_B(record_car, (int32_t)player_records) = (int8_t)local_player;
        MEM_H(0, (int32_t)num_players) = 1;
        MEM_H(0, (int32_t)num_views) = 1;
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
        phase = Phase::Idle;
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

namespace {
    // A notice over the game while it waits on another player ("Waiting for ..."; race_status), empty to close it.
    std::string shown_notice;
    void show_wait_notice(const std::string& text) {
        if (text == shown_notice) {
            return;
        }
        if (text.empty()) {
            recompui::close_prompt();
        }
        else {
            recompui::open_notification("Online Race", text);
        }
        shown_notice = text;
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
    if (phase != Phase::Idle && st != State::Loading && st != State::Racing) {
        // The race ended under us (the host left, or a desync).
        std::fprintf(stderr, "[online] race ended: %s\n", last_error().c_str());
        end_race(rdram, ctx, phase != Phase::WaitMenus);
        return;
    }
    if (phase == Phase::Running && MEM_W(0, (int32_t)game_state) == state_post_race) {
        // Race over: every machine gets here on the same frame and leaves the lockstep. The post-race (saving to the
        // profiles, the track's record tables, name entry) runs on each machine for its own player only.
        std::fprintf(stderr, "[online] race over at frame %u\n", race_frame);
        finish_race_locally(rdram);
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
        report_loaded(hash);
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
        show_wait_notice(race_status());
    }
    show_wait_notice({});
    if (got == Wait::Ended) {
        lockstep = false;
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

    // Two physics ticks, each with its input step.
    recomp_context saved = *ctx;
    ticking = true;
    for (int tick = 0; tick < 2; tick++) {
        physics_thread_step_once_80076D70(rdram, ctx);
        *ctx = saved;
    }
    ticking = false;

    // Every 30 frames the machines compare their states (online_net.cpp ends the race if they differ).
    if (race_frame % 30 == 0) {
        uint32_t hash = state_hash(rdram);
        dump_state(rdram, race_frame);
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

// The pause menu step (func_800AFD44): off in online races.
extern "C" int rush2_online_block_pause(uint8_t* rdram, recomp_context* ctx) {
    return racing() ? 1 : 0;
}

// The race setup (func_800A5ADC at 0x800A5FD0) has just set the view count to the player count, for the split screen
// it sets up next (func_80054BB0): online each machine has one view.
extern "C" void rush2_online_race_views(uint8_t* rdram, recomp_context* ctx) {
    if (phase == Phase::Loading) {
        MEM_H(0, (int32_t)num_views) = 1;
    }
}

bool rush2::online::game::read_local_controller(uint16_t* buttons, float* x, float* y) {
    return input_source != nullptr && input_source(0, buttons, x, y);
}

bool rush2::online::game::in_race() {
    return phase != Phase::Idle;
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
// machine's race stays the same. The engine sounds and rumble (func_80067358, in the physics tick) can't be moved the
// same way: they write the player cars' +0x660, which the physics reads (func_80071D78), so they stay player 1's.
namespace {
    bool presenting = false;
    int8_t saved_car = 0;
    int8_t saved_port = 0;
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
