// Test aid, not in the menus: RUSH2_TEST_RACE starts a race straight from the title screen, with no input script
// through the menus (docs/menus.md#starting-a-race-without-the-menus).
//
// - RUSH2_TEST_RACE=<track select id> (0-11 Rush 2, then the added tracks, include/track2049.h) or the folder name of
//   a custom track (a mod's tracks/<folder>).
// - RUSH2_TEST_RACE_CARS=<type>[,<type>...] (Rush 2 0-21, 2049 23-35, California Speed 36-56, mod cars 57-72): one car
//   per player, 1 to 4 players (split screen). Without it, one player in RUSH2_TEST_CAR's car, or the Pickup (0).
// - RUSH2_TEST_RACE_LAPS=<n>, RUSH2_TEST_RACE_DRONES=<n>: the track select's LAPS and DRONES (the race keeps the saved
//   ones otherwise).
// - RUSH2_TEST_RACE_BATTLE=1: the race select's BATTLE row on (rush2::battle::set_race_battle; not saved).
// - RUSH2_TEST_RACE_VIEW_CAR=<car>: the first view follows that car (a drone: 1 and up) for the whole race, as
//   RUSH2_ONLINE_VIEW_CAR does online, to watch a drone drive the course.
// - RUSH2_TEST_RACE_WAIT=<seconds>: the race starts that long after the title screen is first up (a mod rebuilt at
//   launch finished first, as when a player starts a race after it).
//
// Each player races as JUST PLAY on Select Player has them: with their controller's own buffer (0x8010D740 +
// port * 0x6C0, never a saved player's profile) and its options set to the defaults, as menu_select_player_pick_saved
// does (0x803B4590-0x803B4620). Player p is on controller port p, so an input script (player 1) drives the first car.
// The car select's part is done as it does it: the buffer's selected car (profile_set_selected_car_type_80097F8C, which
// keeps a 2049 or other added type in full, src/rush2049/car2049.cpp), the car table's entry and the car type tables'
// player rows (rush2::race_setup::fill_player_rows). The track goes in as the track select leaves it, through the host
// slot an added track races in (rush2::track2049::host_menu_track). RUSH2_TEST_MANUAL and RUSH2_TEST_DEATHS
// (src/players4.cpp) still apply.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "calspeed.h"
#include "battle.h"
#include "car_types.h"
#include "online.h"
#include "race_setup.h"
#include "track2049.h"
#include "track_mod_runtime.h"

extern "C" void profile_set_selected_car_type_80097F8C(uint8_t* rdram, recomp_context* ctx);
extern "C" void input_set_port_state_80093DC0(uint8_t* rdram, recomp_context* ctx);
extern "C" void music_queue_stop_80062FA4(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_main_color_800B2694(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_accent_color_800B2640(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_options_get_color_lo_800B2608(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_stripe_color_800B25D0(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t player_records = 0x800C2140;    // 0x28 each, +0x24 the profile
    constexpr uint32_t player_record_size = 0x28;
    constexpr int32_t record_profile = 0x24;
    constexpr uint32_t player_ports = 0x800BF314;      // u8 per player: their controller port
    constexpr uint32_t controller_buffers = 0x8010D740; // 0x6C0 per port: the profile JUST PLAY races with
    constexpr uint32_t profile_size = 0x6C0;
    constexpr int32_t profile_options = 0x4C4;         // 0xC0 bytes of options, reset by JUST PLAY
    constexpr uint32_t default_options = 0x80118A50;   // 0xC0 per port: what JUST PLAY resets them to
    constexpr uint32_t options_size = 0xC0;
    constexpr int32_t profile_car = 0x31;              // The selected car type's byte (low 5 bits)
    constexpr uint32_t car_table = 0x800D9C10;         // 9 bytes per car slot: +1 type, +2 main, +3 accent,
    constexpr uint32_t car_entry_size = 9;             // +4 stripe, +5 stripe color
    constexpr uint32_t num_players = 0x8010C3E2;       // s16
    constexpr uint32_t view_cars = 0x80240380;         // u8 per view: the car it follows
    constexpr uint32_t num_views = 0x8010C3EC;         // s16
    constexpr uint32_t race_type = 0x8010C3E8;         // s32: 0 a race, 1 circuit, 2 stunt
    constexpr uint32_t menu_settings = 0x800D5760;     // +0 laps, +1 drones (func_80094698 copies them to the race)
    constexpr int max_players = 4;
    // Track select ids: Rush 2's, the added games', the custom tracks' (38-63) and California Speed's (64-79) last.
    constexpr int menu_tracks = rush2::calspeed::first_menu_id + rush2::calspeed::track_count;

    int call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
             int32_t a1 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = (gpr)a0;
        ctx->r5 = (gpr)a1;
        func(rdram, ctx);
        int result = (int)(int8_t)ctx->r2;
        *ctx = saved;
        return result;
    }

    const char* env(const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr && *v != 0 ? v : nullptr;
    }

    // The track select id RUSH2_TEST_RACE names, or -1.
    int track_from_env(const char* spec) {
        char* end = nullptr;
        long id = std::strtol(spec, &end, 10);
        if (end != spec && *end == 0) {
            return id >= 0 && id < menu_tracks ? (int)id : -1;
        }
        std::vector<rush2::track_mod::Entry> mods = rush2::track_mod::tracks();
        for (size_t i = 0; i < mods.size(); i++) {
            if (mods[i].folder == spec && rush2::track2049::mod_menu_id + int(i) < rush2::calspeed::first_menu_id) {
                return rush2::track2049::mod_menu_id + int(i);
            }
        }
        return -1;
    }

    std::vector<int> cars_from_env() {
        std::vector<int> cars;
        if (const char* list = env("RUSH2_TEST_RACE_CARS")) {
            for (const char* at = list; *at != 0 && (int)cars.size() < max_players;) {
                cars.push_back(std::atoi(at));
                const char* comma = std::strchr(at, ',');
                at = comma != nullptr ? comma + 1 : at + std::strlen(at);
            }
        }
        if (cars.empty()) {
            cars.push_back(env("RUSH2_TEST_CAR") != nullptr ? std::atoi(env("RUSH2_TEST_CAR")) : 0);
        }
        for (int& type : cars) {
            if (type < 0 || type >= rush2::car_types::types || type == 22) {
                std::fprintf(stderr, "[test_race] car type %d doesn't exist; racing the Pickup (0)\n", type);
                type = 0;
            }
        }
        return cars;
    }

    void start(uint8_t* rdram, recomp_context* ctx, int track, const std::vector<int>& cars) {
        int players = (int)cars.size();
        recomp_context saved = *ctx;
        music_queue_stop_80062FA4(rdram, ctx);
        *ctx = saved;
        for (int p = 0; p < players; p++) {
            uint8_t port = (uint8_t)p;
            rush2::race_setup::init_record(rdram, p, port, 1);
            int32_t record = (int32_t)(player_records + p * player_record_size);
            uint32_t profile = controller_buffers + port * profile_size;
            for (uint32_t i = 0; i < options_size; i++) {
                MEM_B(0, (int32_t)(profile + profile_options + i)) = MEM_B(0, (int32_t)(default_options + port * options_size + i));
            }
            MEM_W(record_profile, record) = (int32_t)profile;
            MEM_B(p, (int32_t)player_ports) = (int8_t)port;
            call(rdram, ctx, input_set_port_state_80093DC0, port, 1);
            call(rdram, ctx, profile_set_selected_car_type_80097F8C, (int32_t)(profile + profile_car), cars[p]);
            int32_t entry = (int32_t)(car_table + p * car_entry_size);
            MEM_B(1, entry) = (int8_t)cars[p];
            MEM_B(2, entry) = (int8_t)call(rdram, ctx, car_record_get_main_color_800B2694, (int32_t)profile, cars[p]);
            MEM_B(3, entry) = (int8_t)call(rdram, ctx, car_record_get_accent_color_800B2640, (int32_t)profile, cars[p]);
            MEM_B(4, entry) = (int8_t)call(rdram, ctx, car_options_get_color_lo_800B2608, (int32_t)profile, cars[p]);
            MEM_B(5, entry) = (int8_t)call(rdram, ctx, car_record_get_stripe_color_800B25D0, (int32_t)profile, cars[p]);
        }
        rush2::race_setup::fill_player_rows(rdram, ctx, players);
        MEM_H(0, (int32_t)num_players) = (int16_t)players;
        MEM_H(0, (int32_t)num_views) = (int16_t)players;
        // ONE RACE's game mode: the race start turns a race on a stunt track or arena into stunt mode itself.
        MEM_W(0, (int32_t)race_type) = 0;
        if (const char* laps = env("RUSH2_TEST_RACE_LAPS")) {
            MEM_B(0, (int32_t)menu_settings) = (int8_t)std::atoi(laps);
        }
        if (const char* drones = env("RUSH2_TEST_RACE_DRONES")) {
            MEM_B(1, (int32_t)menu_settings) = (int8_t)std::atoi(drones);
        }
        if (const char* battle = env("RUSH2_TEST_RACE_BATTLE")) {
            rush2::battle::set_race_battle(std::atoi(battle) != 0);
        }
        int slot = rush2::track2049::host_menu_track(rdram, track);
        // View p follows player p's car, as online sets them (src/online/online_game.cpp); the menus leave them so, but
        // from the title screen view 0 kept another car and split screen's first view watched it from afar.
        for (int p = 0; p < players; p++) {
            MEM_B(p, (int32_t)view_cars) = (int8_t)p;
        }
        // Set before the race starts, as online does (src/online/online_game.cpp): the race camera keeps a per-view
        // collision cache from its car, and switching cars mid-race crashes in collision_node_find_cached_8006CE28.
        if (const char* view_car = env("RUSH2_TEST_RACE_VIEW_CAR")) {
            MEM_B(0, (int32_t)view_cars) = (int8_t)std::atoi(view_car);
        }
        rush2::race_setup::leave_front_end(rdram, ctx);
        std::string types;
        for (int type : cars) {
            types += (types.empty() ? "" : ",") + std::to_string(type);
        }
        std::fprintf(stderr, "[test_race] starting track %d (slot %d), %d player(s), cars %s\n", track, slot, players,
                     types.c_str());
    }
}

// Top of the game thread's frame (func_800B0228's loop, after rush2_online_frame), once per frame.
extern "C" void rush2_test_race_frame(uint8_t* rdram, recomp_context* ctx) {
    static const char* spec = env("RUSH2_TEST_RACE");
    static bool done = false;
    if (spec == nullptr || done || rush2::online::state() != rush2::online::State::Offline ||
        !rush2::race_setup::in_menus(rdram)) {
        return;
    }
    // RUSH2_TEST_RACE_WAIT=<seconds>: stay on the title screen that long first (a launch-time rebuild, such as the
    // arcade mods' refresh, finishing before the race).
    static const double wait = env("RUSH2_TEST_RACE_WAIT") ? std::atof(env("RUSH2_TEST_RACE_WAIT")) : 0.0;
    static const auto first = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() < wait) {
        return;
    }
    done = true;
    int track = track_from_env(spec);
    if (track < 0) {
        std::fprintf(stderr, "[test_race] RUSH2_TEST_RACE=%s is no track select id or custom track folder\n", spec);
        return;
    }
    start(rdram, ctx, track, cars_from_env());
}
