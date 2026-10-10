#ifndef __BATTLE_AI_H__
#define __BATTLE_AI_H__

#include <cstdint>
#include <vector>

#include "recomp.h"

// Computer opponents for Rush 2049's battle arenas (src/rush2049/battle_ai.cpp, docs/rush2049_research/battle.md
// section 9). Rush 2049's battle was multiplayer only, so this is new: the opponents are Rush 2's drone cars with
// their driver (func_80074990) replaced, finding their way around the arena on a grid made from its collision and
// fighting with the battle's own weapons (src/rush2049/battle.cpp).
namespace rush2::battle_ai {
    // The computer cars a battle with this many players gets when the track select's DRONES asks for `wanted`.
    int race_opponents(int wanted, int players);

    // The arena's solid triangles (x, y, z per corner), when a battle arena is converted: the navigation grid is
    // built from them on a thread of its own.
    void set_arena(const std::vector<float>& triangles);

    // What the battle knows of the round, given to update() each physics tick.
    struct Car {
        bool present = false;    // in the battle
        bool alive = false;      // and not wrecked
        bool bot = false;        // a computer car
        int team = -1;           // a player's team, -1 for a computer car
        float pos[3] = {};       // where it is drawn (the model's origin, at the bottom of the body)
        float vel[3] = {};
        float m[9] = {};         // rows right, up, forward
        float health = 0.0f;     // of 800
        int weapon = 8;          // 0-7 the pickups' (cannon .. sonic), 8 the default gun
        int ammo = -1;
        float cooldown = 0.0f;   // seconds before it can fire
        float aim_yaw = 0.0f;    // the guns' own aim
        bool invisible = false;
        bool shield = false;
        int last_attacker = -1;  // the car that last damaged it
    };
    struct Pickup {
        float pos[3] = {};
        int kind = 0;            // 0-7 a weapon, 8 heal, 9 invisibility, 10 shield, 11 a random power-up
        bool available = false;
    };
    struct Threat {
        float pos[3] = {};
        float forward[3] = {};   // a missile's heading
        int owner = -1;
        bool missile = false;
        bool mine = false;       // a mine at rest on the ground
    };
    struct World {
        Car cars[8];
        std::vector<Pickup> pickups;
        std::vector<Threat> threats;
        float dt = 0.0f;
        bool over = false;       // the round's time is up
    };
    // A new round (the battle is set up).
    void begin_round();
    // Each physics tick of a battle, before the cars are stepped: the computer cars decide what to do.
    void update(uint8_t* rdram, const World& world);
    // The battle's weapon buttons (rush2::controls::battle_fire / battle_drop) computer car `car` holds this tick.
    uint8_t buttons(int car);
    // Whether `car` is a computer car of the battle in progress.
    bool is_bot(uint8_t* rdram, int car);
    // A computer car's number (1-7) for the results, or 0.
    int bot_number(uint8_t* rdram, int car);
}

#endif
