// Setting a race up without the menus: what the Select Player, track select and car select screens leave for the race
// start (game state 7). Shared by online races (src/online/online_game.cpp) and the RUSH2_TEST_RACE test aid
// (src/test_race.cpp). See docs/menus.md#starting-a-race-without-the-menus.

#include "race_setup.h"
#include "car_types.h"

extern "C" void menu_screen_dispatch_800AE0D4(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_main_color_800B2694(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_accent_color_800B2640(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_options_get_color_lo_800B2608(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_record_get_stripe_color_800B25D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_f_800B259C(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_stat_value_b_800B2468(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_stat_value_a_800B2424(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_d_800B2554(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_c_800B2530(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_b_800B250C(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_a_800B24E8(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_stat_value_c_800B24AC(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_e_800B2578(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;      // 0 menus, 1 track select, 2 car select, 7 race start, 3 racing
    constexpr uint32_t demo_flag = 0x800FAE6C;       // u8: the attract demo is running
    // The front end screen up (func_800AE0D4): 0x10 the title screen, 0 the main menu after it, up to 0xD the other
    // menu screens; 0xE is the boot logos, 0x11 and 0x12 the attract demo, -1 none.
    constexpr uint32_t menu_screen = 0x800E7BC4;     // s16
    constexpr int16_t last_menu_screen = 0xD;
    constexpr int16_t title_screen = 0x10;

    constexpr uint32_t player_records = 0x800C2140;
    constexpr uint32_t player_record_size = 0x28;
    constexpr int32_t record_car = 0x0;
    constexpr int32_t record_port = 0x1;
    constexpr int32_t record_range = 0xA;            // s8 x 2: the stick's range (what full deflection is)
    constexpr int32_t record_masks = 0xC;            // u16 per action: the N64 buttons bound to it
    constexpr int32_t record_masks_size = 0x12;
    constexpr int32_t record_automatic = 0x20;       // u8: TRANSMISSION, 1 automatic
    constexpr int32_t record_profile = 0x24;         // The player's profile (a controller's buffer or a pak profile)
    // The game's stick processing (func_80076A00) starts the range at 0x46 and grows it toward 0x7F.
    constexpr int8_t default_range = 0x46;
}

bool rush2::race_setup::in_menus(uint8_t* rdram) {
    int32_t state = MEM_W(0, (int32_t)game_state);
    int16_t screen = MEM_H(0, (int32_t)menu_screen);
    bool front_end = state == 0 && ((screen >= 0 && screen <= last_menu_screen) || screen == title_screen);
    return (front_end || state == 1 || state == 2) && MEM_BU(0, (int32_t)demo_flag) == 0;
}

void rush2::race_setup::init_record(uint8_t* rdram, int player, uint8_t port, uint8_t automatic) {
    // The menus fill the masks only for players who picked a profile, so a race started from the title screen would
    // have none (no brake, gears or the like). Records 4-7 fit since the globals after the records moved
    // (tools/players4/relocate.py).
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
    int32_t record = (int32_t)(player_records + player * player_record_size);
    for (int32_t i = 0; i < (int32_t)player_record_size; i++) {
        MEM_B(i, record) = 0;
    }
    for (int i = 0; i < record_masks_size / 2; i++) {
        MEM_H(record_masks + i * 2, record) = (int16_t)masks[i];
    }
    MEM_B(record_car, record) = (int8_t)player;
    MEM_B(record_port, record) = (int8_t)port;
    MEM_B(record_automatic, record) = (int8_t)automatic;
    MEM_B(record_range, record) = default_range;
    MEM_B(record_range + 1, record) = default_range;
}

void rush2::race_setup::fill_player_rows(uint8_t* rdram, recomp_context* ctx, int players) {
    namespace ct = rush2::car_types;
    constexpr int types = ct::types;
    constexpr int player_rows = 8;
    struct Table {
        uint32_t address;
        bool is_float;
        void (*get)(uint8_t*, recomp_context*);
    };
    static const Table tables[] = {
        { ct::table(ct::main_color), false, car_record_get_main_color_800B2694 },
        { ct::table(ct::accent_color), false, car_record_get_accent_color_800B2640 },
        { ct::table(ct::stripe), false, car_options_get_color_lo_800B2608 },
        { ct::table(ct::stripe_color), false, car_record_get_stripe_color_800B25D0 },
        { ct::table(ct::engine), false, carselect_lookup_value_d_800B2554 },     // ENGINE
        { ct::table(ct::torque), false, carselect_lookup_value_c_800B2530 },     // Engine torque map
        { ct::table(ct::suspension), false, carselect_lookup_value_b_800B250C },     // Suspension
        { ct::table(ct::tires), false, carselect_lookup_value_a_800B24E8 },     // Handling
        { ct::table(ct::weight), true, carselect_get_stat_value_c_800B24AC },  // Weight (DURABILITY)
        { ct::table(ct::rims), false, carselect_lookup_value_f_800B259C },     // Rims (the wheels' texture).
        { ct::table(ct::horn), false, carselect_lookup_value_e_800B2578 },
        { ct::table(ct::front_wheel_scale), true, carselect_get_stat_value_b_800B2468 },    // Front wheel scale.
        { ct::table(ct::rear_wheel_scale), true, carselect_get_stat_value_a_800B2424 },    // Rear wheel scale.
    };
    recomp_context saved = *ctx;
    for (int p = 0; p < players && p < player_rows; p++) {
        uint32_t profile = (uint32_t)MEM_W(record_profile, (int32_t)(player_records + p * player_record_size));
        int row = p + 1;
        for (const Table& t : tables) {
            for (int type = 0; type < types; type++) {
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

void rush2::race_setup::leave_front_end(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_H(0, (int32_t)menu_screen) >= 0) {
        recomp_context saved = *ctx;
        ctx->r17 = (gpr)(int32_t)MEM_H(0, (int32_t)menu_screen);  // $s1 the screen, $s0 0: it's left
        ctx->r16 = 0;
        menu_screen_dispatch_800AE0D4(rdram, ctx);
        *ctx = saved;
    }
    MEM_H(0, (int32_t)menu_screen) = -1;
    MEM_W(0, (int32_t)game_state) = 7;
}
