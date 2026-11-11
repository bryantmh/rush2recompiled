// DEATHS' EXPLOSIVE value: the SF Rush PlayStation version's Explosive mode (docs/sfrush_ps1.md#explosive-mode).
//
// The PS1 (func_800155F8, its race frame): with Explosive (gp+0x30) on it turns Extreme on and clears the bomb (gp+0x34)
// at the race start (0x80015E24-0x80015E3C); each frame (0x800161A8-0x80016214) the speedometer's mph (0x801106F8,
// written by func_8008C038) at 66 or more arms the bomb, and with the bomb armed a reading under 60 sets the car's wreck
// bit (car +0x352 |= 0x8000) unless the car has finished (0x80101254). The bomb never disarms; the wreck loses the race
// (the EXLOSEN.TGA picture, "Better luck next time!"). Its HUD icon (func_8002EBA0) blinks while the bomb is armed.
//
// Here EXPLOSIVE is a DEATHS race (one life), so the wreck puts the car out as any wreck with DEATHS does. Every human
// player carries a bomb (the PS1's player 1 did; drones never). The speed is the speedometer's mph: car state +0xE0 is mph x 4,
// which hud_speedometer_800BA2A8 shows shifted right by 2 (0x800BA3AC-0x800BA3B8). The armed flags are race state, so online they are part of the resync
// (rush2::explosive::online_sync).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "recomp.h"

#include "arrows.h"
#include "circuit.h"
#include "explosive.h"
#include "online_sync.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint32_t race_deaths = 0x8010BC98;     // s8: DEATHS as the race runs it (0 off, 1 on, 2 EXPLOSIVE)
    constexpr uint32_t menu_settings = 0x800D5760;   // the track select's settings
    constexpr int32_t menu_deaths = 0x11;            // s8: DEATHS (0, 1, 2)
    constexpr uint32_t attract_demo = 0x800FAE6C;    // u8: nonzero while the attract demo runs
    constexpr uint32_t game_clock = 0x80117488;      // f32 seconds
    constexpr uint32_t physics_clock = 0x8010C0E4;   // f32: the race's physics time, 0 at its start
    constexpr uint32_t race_cars = 0x8010C15A;       // s8: the race's cars
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t car_kind = 0x7E8;             // u8: 1 drone, 2 human
    constexpr uint32_t car_wrecked = 0x648;          // s8: set, Rush 2's own wreck runs (the car blows up)
    constexpr uint32_t car_respawn = 0x6C8;          // s16: >= 0 while it glides back to the road
    constexpr uint32_t car_states = 0x801124A0, car_state_size = 0x354;
    constexpr uint32_t state_mph = 0xE0;             // s16: the speedometer's speed, mph x 4 (it shows this >> 2)
    constexpr uint32_t state_finished = 0xEA;        // u8
    constexpr uint32_t state_out = 0x343;            // u8: out of deaths
    constexpr int max_cars = 8;

    std::mutex state_mutex;
    bool armed_cars[max_cars] = {};
    float last_physics = 0.0f;

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }
}

bool rush2::explosive::on(uint8_t* rdram) {
    return MEM_BU(0, (int32_t)race_deaths) == deaths_value && MEM_BU(0, (int32_t)attract_demo) == 0;
}

bool rush2::explosive::armed(int car) {
    if (car < 0 || car >= max_cars) return false;
    std::lock_guard lock{ state_mutex };
    return armed_cars[car];
}

namespace rush2::explosive {
    void online_sync(rush2::online::sync::Walker& w) {
        std::lock_guard lock{ state_mutex };
        rush2::online::sync::field(w, armed_cars);
    }
}

// physics_tick_80076578 at 0x8007660C, the end of a physics tick (every car stepped): the bombs.
extern "C" void rush2_explosive_tick(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::lock_guard lock{ state_mutex };
    float now = read_f(rdram, physics_clock);
    if (now < last_physics) {
        for (bool& a : armed_cars) a = false;    // a new race
    }
    last_physics = now;
    if (!rush2::explosive::on(rdram)) {
        for (bool& a : armed_cars) a = false;
        return;
    }
    int n = std::min(max_cars, std::max(0, (int)(int8_t)MEM_B(0, (int32_t)race_cars)));
    for (int c = 0; c < n; c++) {
        uint32_t car = cars + (uint32_t)c * car_size;
        uint32_t state = car_states + (uint32_t)c * car_state_size;
        if (MEM_BU(0, (int32_t)(car + car_kind)) != 2) continue;
        if (MEM_BU(0, (int32_t)(state + state_finished)) != 0 || MEM_BU(0, (int32_t)(state + state_out)) != 0) continue;
        if (MEM_B(0, (int32_t)(car + car_wrecked)) != 0 || (int16_t)MEM_H(0, (int32_t)(car + car_respawn)) >= 0) continue;
        int mph = std::abs((int)(int16_t)MEM_H(0, (int32_t)(state + state_mph)) >> 2);
        if (mph >= rush2::explosive::arm_mph) armed_cars[c] = true;
        if (armed_cars[c] && mph < rush2::explosive::blow_mph) {
            MEM_B(0, (int32_t)(car + car_wrecked)) = 1;
            std::printf("[explosive] car %d blew up at %d mph\n", c, mph);
            std::fflush(stdout);
        }
    }
}

// menu_settings_to_race_globals_80094698 at 0x80094978, DEATHS just copied to 0x8010BC98: a circuit's comes from one
// bit of its block (0 / 1), so EXPLOSIVE comes from the circuit's record (rush2::circuit::explosive), or the menu's
// DEATHS while the circuit has none (a circuit started before EXPLOSIVE, or one another machine hosts online).
extern "C" void rush2_explosive_race_deaths(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    constexpr uint32_t game_mode = 0x8010C3E8;    // s32: 1 = circuit
    if ((int32_t)MEM_W(0, (int32_t)game_mode) != 1 || MEM_BU(0, (int32_t)race_deaths) == 0) return;
    int circuit = rush2::circuit::explosive(rdram);
    bool on = circuit >= 0 ? circuit == 1 : MEM_BU(menu_deaths, (int32_t)menu_settings) == rush2::explosive::deaths_value;
    MEM_B(0, (int32_t)race_deaths) = (int8_t)(on ? rush2::explosive::deaths_value : 1);
}

// hud_widget_cb_deaths_skull_800B9C44 at 0x800B9CA4, about to show the skull ($a0 the widget, +0x2C its player).
// Returns 1 to hide it this frame instead: the skull blinks while the player's bomb is armed (four times a second, as
// the PS1's icon, which swaps every 4 frames of its counter 0x800B3384). The hide (hud_widget_init_type_80060DA8 with
// 1) sets the widget's hidden byte +0x1A, which the game's show path (hud_widget_reset_80060104) leaves set, so a
// frame that shows it clears the byte.
extern "C" int rush2_explosive_skull_hidden(uint8_t* rdram, recomp_context* ctx) {
    constexpr int32_t widget_hidden = 0x1A;    // u8
    int32_t w = (int32_t)ctx->r4;
    bool hide = false;
    if (rush2::explosive::on(rdram)) {
        int player = (int32_t)MEM_W(0x2C, w);
        if (rush2::explosive::armed(rush2::views::player_car(rdram, player))) {
            hide = std::fmod(read_f(rdram, game_clock), 0.25f) >= 0.125f;
        }
    }
    if (!hide) MEM_B(widget_hidden, w) = 0;
    return hide ? 1 : 0;
}
