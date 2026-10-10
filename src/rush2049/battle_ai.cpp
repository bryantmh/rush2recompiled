// Computer opponents for Rush 2049's battle arenas (docs/rush2049_research/battle.md section 9).
//
// Rush 2049's battle was multiplayer only, so nothing here is ported: it is written for the port, on top of the
// battle's own rules (src/rush2049/battle.cpp). Its layers:
//
// - Cars. The opponents are Rush 2's drone cars (car +0x7E8 == 1): the race settings give a battle the track select's
//   DRONES (kept open on the BATTLE select) as its drone count (0x800D3E90, set at the end of func_80094698, where the
//   arenas' stunt mode had none), and the drone slots get their cars and colors as in any race (src/npc_cars.cpp's choices included). A
//   drone's driver func_80074990 follows the race path; for an opponent it is skipped and this file writes the car's
//   inputs as a player's would be: +0x728 steering (-1 left .. 1 right), +0x734 throttle and +0x730 brake (0-1),
//   +0x738 gear (1 drive, -1 reverse; a drone's transmission is automatic). func_80075C3C steps drones round robin, a
//   few each physics tick, so like the ghosts (src/ghost.cpp) an opponent's scheduled step is skipped and it is
//   stepped every tick after the others, as a player's car is: it handles, collides and fires at the players' rate.
// - Navigation. The arena's solid triangles (the same ones its shots hit) are sampled on a grid over x and z: in each
//   column every surface no steeper than 50 degrees with room for a car above it is a node, and a node links to the
//   nodes of its 8 neighbor columns that are within a slope's rise of it (or below it: a car can drop off a ledge,
//   one way) with no steep triangle in between at bumper height. Nodes near a wall cost more, so routes keep off the
//   walls. A* over the links finds a route; the car steers for a point some way along it (pure pursuit), or straight
//   at its goal when the way there is open and level.
// - Goals. A few times a second (more often the higher the skill) each opponent scores its options: each enemy car
//   (close, damaged, the one that last hit it, the one it is already after), each pickup it can use (a weapon when it
//   only has the gun, health when it is hurt, the power-ups), or roaming the arena, and takes the best. The current
//   goal gets a bonus so it doesn't flip between two. They spread out: a car other opponents are after or one in a
//   crowd scores less, a pickup another opponent is closer to and going for is left to it, and a car roams to a place
//   away from the others. Each opponent draws its own leanings at the round's start (how keen it is to fight or to
//   collect, the range it shoots from, when it keeps away hurt, its pace).
// - Weapons. Each weapon has its own rule for when to fire: the gun and gatling when the guns' own aim
//   (battle.cpp's update_aim) is on the target, the cannon and rockets when the car points at it (with the rocket's
//   flight time led), missiles when the target is in their homing cone, grenades within their throw, the sonic blast
//   with a car close by, mines when a car is behind. The ram has no button: the car drives into its target.
// - Driving. Speed is set from how far the car must turn. Shooting at a car, it holds its range back at that car's
//   speed once it faces it, and keeps going round at a turning speed until then; with a weapon that needn't point
//   straight at it (guns, missiles, grenades) it heads for a point beside it, circling it. It steers round other cars
//   close ahead. A car that has stopped while trying to go reverses out with opposite lock, and one that has had to
//   twice in a short while leaves the cars alone for a few seconds; a car that can't get going for long (on its roof,
//   wedged) is wrecked, and the game respawns it as it does any wrecked car (in a battle, at the route point farthest
//   from the other cars: src/rush2049/track2049.cpp). With the dodge skill a car swerves from a missile coming at it.
//
// Skill (the BATTLE track select's DIFFICULTY, 0-5) sets how often a car thinks, its top speed, how close its aim must be and how often it takes a
// shot, and whether it dodges.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "battle.h"
#include "battle_ai.h"
#include "ghost.h"
#include "rush2.h"

extern "C" void physics_car_tick_80075880(uint8_t* rdram, recomp_context* ctx);       // A car's physics tick ($a0 car, $a1 clock bits).
extern "C" void car_material_effects_update_800663CC(uint8_t* rdram, recomp_context* ctx); // After a drone's tick, when 0x800D042C is set.
extern "C" void player_join_init_car_dynamics_800806A4(uint8_t* rdram, recomp_context* ctx); // A player's clutch (+0x72C) from rpm and throttle.

namespace {
    // Game addresses (Rush 2).
    constexpr uint32_t cars = 0x800F5470;            // physics cars, 0x81C each
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_wrecked = 0x648;          // s8
    constexpr uint32_t car_release = 0x71C;          // s16: nonzero once the car may drive (GO)
    constexpr uint32_t car_steer = 0x728;            // f32 -1 .. 1
    constexpr uint32_t car_throttle = 0x734;         // f32 0 .. 1 (copied to +0x3B4, which the engine torque reads)
    constexpr uint32_t car_brake = 0x730;            // f32 0 .. 1 (+0x3B8)
    constexpr uint32_t car_gear = 0x738;             // s16: 1 drive, -1 reverse
    constexpr uint32_t car_active = 0x7E4;           // s16
    constexpr uint32_t car_kind = 0x7E8;             // u8: 1 drone, 2 human
    constexpr uint32_t physics_clock = 0x8010C0E4;   // f32: the physics tick's time (func_80076578)
    constexpr uint32_t drone_extra = 0x800D042C;     // u8: func_800663CC runs after a drone's step
    constexpr uint32_t game_state = 0x8010C0D0;      // 3 a race, 10 its start countdown
    constexpr int max_cars = 8;
    constexpr uint8_t kind_drone = 1, kind_human = 2;

    // Weapons and pickup kinds (battle.cpp's).
    enum Weapon { cannon, gatling, grenade, mine, missile, ram, rocket, sonic, gun };
    enum Kind { kind_heal = 8, kind_invisibility = 9, kind_shield = 10, kind_powerup = 11 };
    constexpr float max_health = 800.0f;

    constexpr uint32_t race_difficulty = 0x8010C211; // u8 0-5: the track select's DIFFICULTY (menu settings +0xA)

    float read_f(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void write_f(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)addr) = w;
    }

    uint32_t car_at(int i) { return cars + (uint32_t)i * car_size; }
    uint32_t car_state_at(int i) { return 0x801124A0u + (uint32_t)i * 0x354u; }   // draw states (+0x343 the respawn state)

    float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float length(const float a[3]) { return std::sqrt(dot(a, a)); }
    float flat_distance(const float a[3], const float b[3]) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

    float distance(const float a[3], const float b[3]) {
        float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
        return length(d);
    }

    void cross(const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    // The vector v in the frame m (rows right, up, forward).
    void to_local(const float m[9], const float v[3], float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = dot(&m[k * 3], v);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Skill

    struct SkillInfo {
        float think;        // seconds between decisions
        float speed;        // share of the top speed
        float aim;          // how far off the aim may be, times the car's half width at the target's distance
        float fire_chance;  // of taking a shot it could
        bool dodge;         // swerves from missiles
        float reach;        // how far it looks for pickups
    };
    // At DIFFICULTY 0, 2.5 and 5; the steps between blend them. Cars dodge from DIFFICULTY 2.
    const SkillInfo skills[3] = {
        { 0.7f, 0.78f, 2.0f, 0.45f, false, 250.0f },
        { 0.4f, 0.9f, 1.3f, 0.8f, true, 400.0f },
        { 0.15f, 1.0f, 0.9f, 1.0f, true, 600.0f },
    };

    SkillInfo skill_at(int difficulty) {
        float k = std::clamp((float)difficulty / 2.5f, 0.0f, 2.0f);
        int lo = std::min((int)k, 1);
        float f = k - (float)lo;
        const SkillInfo &a = skills[lo], &b = skills[lo + 1];
        auto mix = [f](float x, float y) { return x + (y - x) * f; };
        return { mix(a.think, b.think), mix(a.speed, b.speed), mix(a.aim, b.aim), mix(a.fire_chance, b.fire_chance),
                 difficulty >= 2, mix(a.reach, b.reach) };
    }
    constexpr float top_speed = 130.0f;   // ft/s (about 89 mph): the arenas are small

    // ------------------------------------------------------------------------------------------------------------
    // Navigation

    struct Triangle {
        float a[3], b[3], c[3];
        float ny;           // |normal y|: 1 level, 0 a wall
    };

    // The arena's triangles in a grid of buckets over x and z.
    struct Triangles {
        std::vector<Triangle> list;
        std::unordered_map<int64_t, std::vector<int>> buckets;
        static constexpr float bucket = 16.0f;

        static int64_t key(int x, int z) { return ((int64_t)x << 32) ^ (uint32_t)z; }

        void build(const std::vector<float>& t) {
            for (size_t i = 0; i + 9 <= t.size(); i += 9) {
                Triangle tri;
                memcpy(tri.a, &t[i], 12);
                memcpy(tri.b, &t[i + 3], 12);
                memcpy(tri.c, &t[i + 6], 12);
                float e1[3] = { tri.b[0] - tri.a[0], tri.b[1] - tri.a[1], tri.b[2] - tri.a[2] };
                float e2[3] = { tri.c[0] - tri.a[0], tri.c[1] - tri.a[1], tri.c[2] - tri.a[2] };
                float n[3];
                cross(e1, e2, n);
                float l = length(n);
                if (l < 1e-6f) continue;
                tri.ny = std::fabs(n[1]) / l;
                int x0 = (int)std::floor(std::min({ tri.a[0], tri.b[0], tri.c[0] }) / bucket);
                int x1 = (int)std::floor(std::max({ tri.a[0], tri.b[0], tri.c[0] }) / bucket);
                int z0 = (int)std::floor(std::min({ tri.a[2], tri.b[2], tri.c[2] }) / bucket);
                int z1 = (int)std::floor(std::max({ tri.a[2], tri.b[2], tri.c[2] }) / bucket);
                if (x1 - x0 > 256 || z1 - z0 > 256) continue;
                int index = (int)list.size();
                list.push_back(tri);
                for (int x = x0; x <= x1; x++) {
                    for (int z = z0; z <= z1; z++) buckets[key(x, z)].push_back(index);
                }
            }
        }

        // The heights (and steepness) of every triangle over the point (x, z).
        void column(float x, float z, std::vector<std::pair<float, float>>& out) const {
            out.clear();
            auto it = buckets.find(key((int)std::floor(x / bucket), (int)std::floor(z / bucket)));
            if (it == buckets.end()) return;
            for (int index : it->second) {
                const Triangle& t = list[index];
                float d = (t.b[0] - t.a[0]) * (t.c[2] - t.a[2]) - (t.c[0] - t.a[0]) * (t.b[2] - t.a[2]);
                if (std::fabs(d) < 1e-6f) continue;
                float u = ((x - t.a[0]) * (t.c[2] - t.a[2]) - (t.c[0] - t.a[0]) * (z - t.a[2])) / d;
                float v = ((t.b[0] - t.a[0]) * (z - t.a[2]) - (x - t.a[0]) * (t.b[2] - t.a[2])) / d;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
                float y = t.a[1] + u * (t.b[1] - t.a[1]) + v * (t.c[1] - t.a[1]);
                out.push_back({ y, t.ny });
            }
            std::sort(out.begin(), out.end());
        }

        // Whether the segment p0-p1 crosses a triangle steeper than `max_ny` (a wall).
        bool blocked(const float p0[3], const float p1[3], float max_ny = 0.6f) const {
            float d[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            int x0 = (int)std::floor(std::min(p0[0], p1[0]) / bucket), x1 = (int)std::floor(std::max(p0[0], p1[0]) / bucket);
            int z0 = (int)std::floor(std::min(p0[2], p1[2]) / bucket), z1 = (int)std::floor(std::max(p0[2], p1[2]) / bucket);
            if (x1 - x0 > 32 || z1 - z0 > 32) return true;
            for (int x = x0; x <= x1; x++) {
                for (int z = z0; z <= z1; z++) {
                    auto cell = buckets.find(key(x, z));
                    if (cell == buckets.end()) continue;
                    for (int index : cell->second) {
                        const Triangle& t = list[index];
                        if (t.ny > max_ny) continue;
                        float e1[3] = { t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2] };
                        float e2[3] = { t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2] };
                        float p[3], q[3];
                        cross(d, e2, p);
                        float det = dot(e1, p);
                        if (std::fabs(det) < 1e-7f) continue;
                        float s[3] = { p0[0] - t.a[0], p0[1] - t.a[1], p0[2] - t.a[2] };
                        float u = dot(s, p) / det;
                        if (u < 0.0f || u > 1.0f) continue;
                        cross(s, e1, q);
                        float v = dot(d, q) / det;
                        if (v < 0.0f || u + v > 1.0f) continue;
                        float k = dot(e2, q) / det;
                        if (k >= 0.0f && k <= 1.0f) return true;
                    }
                }
            }
            return false;
        }
    };

    constexpr float floor_ny = 0.64f;       // about 50 degrees: steeper is a wall
    constexpr float car_room = 3.5f;        // the headroom a car needs over a surface
    constexpr float bumper = 2.0f;          // the height walls are tested at
    constexpr float max_drop = 30.0f;       // a car drives off a ledge this high
    constexpr int dirs = 8;
    constexpr int dir_x[dirs] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    constexpr int dir_z[dirs] = { 0, 1, 1, 1, 0, -1, -1, -1 };

    struct Nav {
        Triangles tris;
        float x0 = 0.0f, z0 = 0.0f, cell = 4.0f;
        int nx = 0, nz = 0;
        std::vector<int> column_start;   // nx * nz + 1: the column's first node
        std::vector<float> node_y;
        std::vector<int> node_column;
        std::vector<int> links;          // nodes * 8: the linked node each way, or -1
        std::vector<uint8_t> drop;       // nodes * 8: the link drops off a ledge
        std::vector<uint8_t> wall_distance;   // in cells, to a node missing a link (up to 4)
        std::vector<int> component;      // nodes linked either way share one

        int column_of(float x, float z) const {
            int ix = (int)std::floor((x - x0) / cell), iz = (int)std::floor((z - z0) / cell);
            if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return -1;
            return iz * nx + ix;
        }

        void node_pos(int n, float out[3]) const {
            int c = node_column[n];
            out[0] = x0 + ((float)(c % nx) + 0.5f) * cell;
            out[1] = node_y[n];
            out[2] = z0 + ((float)(c / nx) + 0.5f) * cell;
        }

        int nodes() const { return (int)node_y.size(); }

        // The node under a point: in its column the surface a little below it, else the nearest close by.
        int locate(const float p[3]) const {
            int best = -1;
            float best_d = 1e9f;
            int ix = (int)std::floor((p[0] - x0) / cell), iz = (int)std::floor((p[2] - z0) / cell);
            for (int ring = 0; ring <= 3 && best < 0; ring++) {
                for (int dz = -ring; dz <= ring; dz++) {
                    for (int dx = -ring; dx <= ring; dx++) {
                        if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                        int cx = ix + dx, cz = iz + dz;
                        if (cx < 0 || cz < 0 || cx >= nx || cz >= nz) continue;
                        int c = cz * nx + cx;
                        for (int n = column_start[c]; n < column_start[c + 1]; n++) {
                            float dy = p[1] - node_y[n];
                            if (dy < -6.0f || dy > 12.0f) continue;
                            float d = std::fabs(dy) + (float)ring * cell;
                            if (d < best_d) {
                                best_d = d;
                                best = n;
                            }
                        }
                    }
                }
            }
            return best;
        }

        // Whether a car can drive straight from a to b: open (no wall at bumper height) with ground all the way
        // that rises no faster than a slope and doesn't fall away.
        bool straight(const float a[3], const float b[3]) const {
            float dist = flat_distance(a, b);
            if (dist > 400.0f) return false;
            float p0[3] = { a[0], a[1] + bumper, a[2] }, p1[3] = { b[0], b[1] + bumper, b[2] };
            if (tris.blocked(p0, p1)) return false;
            int steps = std::max(1, (int)(dist / (cell * 0.75f)));
            float last = a[1];
            for (int s = 1; s <= steps; s++) {
                float k = (float)s / (float)steps;
                float p[3] = { a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k };
                int c = column_of(p[0], p[2]);
                if (c < 0) return false;
                bool ground = false;
                for (int n = column_start[c]; n < column_start[c + 1]; n++) {
                    float dy = node_y[n] - last;
                    if (dy <= cell * 1.2f && dy >= -cell * 1.5f) {
                        ground = true;
                        last = node_y[n];
                        break;
                    }
                }
                if (!ground) return false;
            }
            return true;
        }
    };

    std::shared_ptr<const Nav> build_nav(const std::vector<float>& triangles) {
        auto nav = std::make_shared<Nav>();
        nav->tris.build(triangles);
        if (nav->tris.list.empty()) return nullptr;
        // The extent of the level surfaces (the walls and sky may reach further).
        float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
        for (const Triangle& t : nav->tris.list) {
            if (t.ny < floor_ny) continue;
            for (const float* v : { t.a, t.b, t.c }) {
                lo[0] = std::min(lo[0], v[0]);
                lo[1] = std::min(lo[1], v[2]);
                hi[0] = std::max(hi[0], v[0]);
                hi[1] = std::max(hi[1], v[2]);
            }
        }
        if (hi[0] <= lo[0] || hi[1] <= lo[1]) return nullptr;
        nav->cell = std::max(4.0f, std::max(hi[0] - lo[0], hi[1] - lo[1]) / 300.0f);
        nav->x0 = lo[0];
        nav->z0 = lo[1];
        nav->nx = (int)std::ceil((hi[0] - lo[0]) / nav->cell) + 1;
        nav->nz = (int)std::ceil((hi[1] - lo[1]) / nav->cell) + 1;
        int columns = nav->nx * nav->nz;
        nav->column_start.assign(columns + 1, 0);
        std::vector<std::pair<float, float>> hits;
        for (int c = 0; c < columns; c++) {
            nav->column_start[c] = (int)nav->node_y.size();
            float x = nav->x0 + ((float)(c % nav->nx) + 0.5f) * nav->cell;
            float z = nav->z0 + ((float)(c / nav->nx) + 0.5f) * nav->cell;
            nav->tris.column(x, z, hits);
            for (size_t i = 0; i < hits.size(); i++) {
                if (hits[i].second < floor_ny) continue;
                // Coincident surfaces (both faces of a floor) are one.
                if (i + 1 < hits.size() && hits[i + 1].first - hits[i].first < 0.25f && hits[i + 1].second >= floor_ny) continue;
                float above = 1e9f;
                for (size_t j = i + 1; j < hits.size(); j++) {
                    if (hits[j].first - hits[i].first >= 0.25f) {
                        above = hits[j].first;
                        break;
                    }
                }
                if (above - hits[i].first < car_room) continue;
                nav->node_y.push_back(hits[i].first);
                nav->node_column.push_back(c);
            }
        }
        nav->column_start[columns] = (int)nav->node_y.size();
        int count = nav->nodes();
        nav->links.assign((size_t)count * dirs, -1);
        nav->drop.assign((size_t)count * dirs, 0);
        for (int n = 0; n < count; n++) {
            int c = nav->node_column[n];
            int ix = c % nav->nx, iz = c / nav->nx;
            float a[3];
            nav->node_pos(n, a);
            for (int d = 0; d < dirs; d++) {
                int jx = ix + dir_x[d], jz = iz + dir_z[d];
                if (jx < 0 || jz < 0 || jx >= nav->nx || jz >= nav->nz) continue;
                int cj = jz * nav->nx + jx;
                float step = nav->cell * ((d & 1) ? 1.41421356f : 1.0f);
                int best = -1;
                float best_dy = 1e9f;
                for (int m = nav->column_start[cj]; m < nav->column_start[cj + 1]; m++) {
                    float dy = nav->node_y[m] - a[1];
                    if (dy > step * 1.2f || dy < -max_drop) continue;
                    if (std::fabs(dy) < std::fabs(best_dy)) {
                        best_dy = dy;
                        best = m;
                    }
                }
                if (best < 0) continue;
                float b[3];
                nav->node_pos(best, b);
                bool is_drop = best_dy < -step * 1.5f;
                // Walls between, at bumper height (a drop: level over the ledge).
                float p0[3] = { a[0], a[1] + bumper, a[2] };
                float p1[3] = { b[0], (is_drop ? a[1] : b[1]) + bumper, b[2] };
                if (nav->tris.blocked(p0, p1)) continue;
                nav->links[(size_t)n * dirs + d] = best;
                nav->drop[(size_t)n * dirs + d] = is_drop;
            }
        }
        // Distance to the nearest node missing a level link, in cells (breadth first from those nodes).
        nav->wall_distance.assign(count, 4);
        std::vector<int> queue;
        for (int n = 0; n < count; n++) {
            for (int d = 0; d < dirs; d++) {
                size_t l = (size_t)n * dirs + d;
                if (nav->links[l] < 0 || nav->drop[l]) {
                    nav->wall_distance[n] = 0;
                    queue.push_back(n);
                    break;
                }
            }
        }
        for (size_t q = 0; q < queue.size(); q++) {
            int n = queue[q];
            for (int d = 0; d < dirs; d += 2) {
                int m = nav->links[(size_t)n * dirs + d];
                if (m >= 0 && nav->wall_distance[m] > nav->wall_distance[n] + 1) {
                    nav->wall_distance[m] = (uint8_t)(nav->wall_distance[n] + 1);
                    queue.push_back(m);
                }
            }
        }
        // Components, taking links either way.
        std::vector<std::vector<int>> back(count);
        for (int n = 0; n < count; n++) {
            for (int d = 0; d < dirs; d++) {
                int m = nav->links[(size_t)n * dirs + d];
                if (m >= 0) back[m].push_back(n);
            }
        }
        nav->component.assign(count, -1);
        int components = 0;
        for (int start = 0; start < count; start++) {
            if (nav->component[start] >= 0) continue;
            queue.assign(1, start);
            nav->component[start] = components;
            for (size_t q = 0; q < queue.size(); q++) {
                int n = queue[q];
                auto visit = [&](int m) {
                    if (m >= 0 && nav->component[m] < 0) {
                        nav->component[m] = components;
                        queue.push_back(m);
                    }
                };
                for (int d = 0; d < dirs; d++) visit(nav->links[(size_t)n * dirs + d]);
                for (int m : back[n]) visit(m);
            }
            components++;
        }
        printf("[BattleAI] Navigation grid: %d x %d columns of %.1f ft, %d nodes, %d components\n", nav->nx, nav->nz,
               nav->cell, count, components);
        fflush(stdout);
        // Test aid: R2_BATTLE_NAV=<file.pgm> writes the grid as an image (white: open, gray: near a wall, black:
        // nothing; the highest node of each column).
        if (const char* path = getenv("R2_BATTLE_NAV"); path != nullptr) {
            if (FILE* f = fopen(path, "wb")) {
                fprintf(f, "P5\n%d %d\n255\n", nav->nx, nav->nz);
                for (int iz = nav->nz - 1; iz >= 0; iz--) {
                    for (int ix = 0; ix < nav->nx; ix++) {
                        int c = iz * nav->nx + ix;
                        uint8_t v = 0;
                        if (nav->column_start[c + 1] > nav->column_start[c]) {
                            int n = nav->column_start[c + 1] - 1;
                            v = (uint8_t)(60 + nav->wall_distance[n] * 48);
                        }
                        fputc(v, f);
                    }
                }
                fclose(f);
            }
        }
        return nav;
    }

    std::mutex nav_mutex;
    std::shared_ptr<const Nav> nav_ready;   // the arena's grid once built
    std::atomic_int nav_generation = 0;

    std::shared_ptr<const Nav> current_nav() {
        std::lock_guard lock{ nav_mutex };
        return nav_ready;
    }

    // A* over the links. Marks within `avoid` of the given points (enemy mines) cost more.
    struct Search {
        std::vector<float> g;
        std::vector<int> from;
        std::vector<uint32_t> seen;
        uint32_t stamp = 0;
    };

    bool find_path(const Nav& nav, Search& s, int start, int goal, const std::vector<std::array<float, 3>>& avoid,
                   std::vector<float>& out) {
        out.clear();
        int count = nav.nodes();
        if (start < 0 || goal < 0 || nav.component[start] != nav.component[goal]) return false;
        if ((int)s.g.size() != count) {
            s.g.assign(count, 0.0f);
            s.from.assign(count, -1);
            s.seen.assign(count, 0);
            s.stamp = 0;
        }
        s.stamp++;
        float goal_pos[3];
        nav.node_pos(goal, goal_pos);
        auto heuristic = [&](int n) {
            float p[3];
            nav.node_pos(n, p);
            float dx = std::fabs(p[0] - goal_pos[0]), dz = std::fabs(p[2] - goal_pos[2]);
            return std::max(dx, dz) + 0.41421356f * std::min(dx, dz);
        };
        using Entry = std::pair<float, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        s.g[start] = 0.0f;
        s.from[start] = -1;
        s.seen[start] = s.stamp;
        open.push({ heuristic(start), start });
        int expanded = 0;
        static const float wall_cost[5] = { 3.0f, 1.2f, 0.4f, 0.1f, 0.0f };
        while (!open.empty() && expanded < 60000) {
            auto [f, n] = open.top();
            open.pop();
            if (n == goal) break;
            if (f - heuristic(n) > s.g[n] + 1e-3f) continue;
            expanded++;
            for (int d = 0; d < dirs; d++) {
                size_t l = (size_t)n * dirs + d;
                int m = nav.links[l];
                if (m < 0) continue;
                float step = nav.cell * ((d & 1) ? 1.41421356f : 1.0f);
                float cost = step * (1.0f + wall_cost[std::min<int>(nav.wall_distance[m], 4)]) + (nav.drop[l] ? 30.0f : 0.0f);
                if (!avoid.empty()) {
                    float p[3];
                    nav.node_pos(m, p);
                    for (const auto& a : avoid) {
                        if (flat_distance(p, a.data()) < 18.0f && std::fabs(p[1] - a[1]) < 8.0f) cost += 60.0f;
                    }
                }
                float g = s.g[n] + cost;
                if (s.seen[m] == s.stamp && g >= s.g[m]) continue;
                s.seen[m] = s.stamp;
                s.g[m] = g;
                s.from[m] = n;
                open.push({ g + heuristic(m), m });
            }
        }
        if (s.seen[goal] != s.stamp) return false;
        std::vector<int> nodes;
        for (int n = goal; n >= 0; n = s.from[n]) {
            nodes.push_back(n);
            if (n == start) break;
        }
        std::reverse(nodes.begin(), nodes.end());
        for (int n : nodes) {
            float p[3];
            nav.node_pos(n, p);
            out.insert(out.end(), { p[0], p[1], p[2] });
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The opponents

    enum class Goal { none, attack, pickup, roam };

    // Each opponent's own leanings, drawn at the round's start, so they don't all play alike.
    struct Personality {
        float aggression = 0.0f;   // added to its score for attacking (-15 .. 15)
        float greed = 0.0f;        // added to its score for pickups (-15 .. 15)
        float range = 40.0f;       // how far back it holds from a car it shoots at (25 .. 55 ft)
        float caution = 0.3f;      // the share of its health under which it keeps away from healthier cars
        float pace = 1.0f;         // share of the skill's top speed (0.9 .. 1)
    };

    struct Brain {
        Personality p;
        float side = 1.0f;           // which side of its target it circles to (1 right, -1 left)
        float side_time = 0.0f;      // seconds until it changes side
        float pinned = 0.0f;         // seconds the car has had to back out lately
        float disengage = 0.0f;      // seconds it keeps away from cars after getting pinned
        Goal goal = Goal::none;
        int target = -1;             // the car attacked
        int pickup = -1;             // the pickup sought
        float roam_to[3] = {};
        float think = 0.0f;          // seconds until the next decision
        std::vector<float> path;     // the route, x y z per point
        size_t path_at = 0;          // the point it is heading past
        float replan = 0.0f;
        int path_goal = -1;          // the node the route leads to
        float stuck = 0.0f, reverse = 0.0f, reverse_steer = 0.0f;
        float idle = 0.0f;           // seconds without getting anywhere
        float idle_from[3] = {};
        float evade = 0.0f, evade_turn = 0.0f;
        uint8_t held = 0;            // the weapon buttons held last tick
        std::vector<float> unreachable;   // per pickup: seconds before it is tried again
        float target_unreachable[max_cars] = {};
        float steer = 0.0f, throttle = 0.0f, brake = 0.0f;
        int gear = 1;
        uint8_t buttons = 0;
    };

    std::mutex ai_mutex;
    Brain brains[max_cars];
    Search search;
    std::mt19937 rng{ 4977 };
    bool round_live = false;
    float ai_clock = 0.0f;       // seconds of the round (the test log's)
    bool stepping[max_cars] = {};

    float random_unit() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); }

    bool test_flag(const char* flag) {
        const char* test = getenv("R2_BATTLE_TEST");
        return test != nullptr && strstr(test, flag) != nullptr;
    }

    // Whether car i is a computer car of a battle: a drone in a battle arena.
    bool bot_car(uint8_t* rdram, int i) {
        if (i < 0 || i >= max_cars || !rush2::battle::active(rdram)) return false;
        uint32_t car = car_at(i);
        return MEM_BU(0, (int32_t)(car + car_kind)) == kind_drone && MEM_H(0, (int32_t)(car + car_active)) != 0 &&
               !rush2::ghost::is_ghost_car(i);
    }

    bool enemies(const rush2::battle_ai::World& w, int a, int b) {
        if (a == b) return false;
        return w.cars[a].team < 0 || w.cars[a].team != w.cars[b].team;
    }

    // The way to `dest` from car i: a point to steer for. Straight at it when the way is open, else along a route.
    bool steer_point(const Nav* nav, Brain& b, const rush2::battle_ai::Car& me, const float dest[3],
                     const std::vector<std::array<float, 3>>& avoid, float dt, float out[3]) {
        memcpy(out, dest, sizeof(float) * 3);
        if (nav == nullptr) return true;
        if (flat_distance(me.pos, dest) < 60.0f && std::fabs(dest[1] - me.pos[1]) < 6.0f && nav->straight(me.pos, dest)) {
            b.path.clear();
            return true;
        }
        int start = nav->locate(me.pos), goal = nav->locate(dest);
        if (goal < 0) return nav->straight(me.pos, dest);
        // Off the grid (in the air off a jump): it keeps to the route it had, or heads straight for the goal.
        if (start < 0 && b.path.empty()) return true;
        b.replan -= dt;
        bool off_route = false;
        if (!b.path.empty() && b.path_at < b.path.size() / 3) {
            off_route = flat_distance(me.pos, &b.path[b.path_at * 3]) > nav->cell * 8.0f;
        }
        if (start >= 0 && (b.path.empty() || goal != b.path_goal || b.replan <= 0.0f || off_route)) {
            b.replan = 0.6f;
            b.path_goal = goal;
            b.path_at = 0;
            if (!find_path(*nav, search, start, goal, avoid, b.path)) {
                b.path.clear();
                return nav->straight(me.pos, dest);
            }
        }
        size_t points = b.path.size() / 3;
        // Past the points behind the car: the nearest of the next few.
        size_t best = b.path_at;
        float best_d = 1e9f;
        for (size_t i = b.path_at; i < std::min(points, b.path_at + 12); i++) {
            float d = flat_distance(me.pos, &b.path[i * 3]);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        b.path_at = best;
        // Then a point some way on (further at speed).
        float speed = std::hypot(me.vel[0], me.vel[2]);
        float ahead = std::clamp(speed * 0.35f, nav->cell * 3.0f, nav->cell * 10.0f);
        float walked = 0.0f;
        size_t i = best;
        while (i + 1 < points && walked < ahead) {
            walked += flat_distance(&b.path[i * 3], &b.path[(i + 1) * 3]);
            i++;
        }
        if (i + 1 >= points && walked < ahead) {
            memcpy(out, dest, sizeof(float) * 3);
        }
        else {
            memcpy(out, &b.path[i * 3], sizeof(float) * 3);
        }
        return true;
    }

    // Where a shot that takes `flight` seconds should be aimed to meet the target.
    void lead(const rush2::battle_ai::Car& target, float flight, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = target.pos[k] + target.vel[k] * flight;
    }

    // Whether car i should fire its weapon at target t now (battle.cpp's weapons: speeds, cones, ranges).
    bool should_fire(const Nav* nav, const SkillInfo& skill, const rush2::battle_ai::World& w, int i, int t) {
        const auto& me = w.cars[i];
        const auto& target = w.cars[t];
        float d = distance(me.pos, target.pos);
        float to[3] = { target.pos[0] - me.pos[0], target.pos[1] - me.pos[1], target.pos[2] - me.pos[2] };
        float local[3];
        to_local(me.m, to, local);
        float angle = std::atan2(local[0], local[2]);
        float half = 3.0f * skill.aim;
        float window = std::atan2(half, std::max(d, 1.0f));
        // The way must be open.
        if (nav != nullptr && d > 8.0f) {
            float p0[3] = { me.pos[0], me.pos[1] + 2.5f, me.pos[2] }, p1[3] = { target.pos[0], target.pos[1] + 2.0f, target.pos[2] };
            if (me.weapon != sonic && me.weapon != mine && me.weapon != grenade && nav->tris.blocked(p0, p1)) return false;
        }
        switch (me.weapon) {
            case gun:
            case gatling:
                // The guns turn themselves toward the nearest car ahead; fire once they are on this one.
                return d < 350.0f && local[2] > 0.0f && std::fabs(angle - me.aim_yaw) < window;
            case cannon:
                return d < 400.0f && local[2] > 0.0f && std::fabs(angle) < window;
            case rocket: {
                // 50 ft/s plus the car's speed, gaining 800 ft/s each second.
                float flight = std::sqrt(2.0f * d / 800.0f);
                float aim[3], rel[3], l[3];
                lead(target, flight, aim);
                for (int k = 0; k < 3; k++) rel[k] = aim[k] - me.pos[k];
                to_local(me.m, rel, l);
                return d < 400.0f && l[2] > 0.0f && std::fabs(std::atan2(l[0], l[2])) < window;
            }
            case missile:
                // It homes on the nearest car in a cone a quarter turn wide.
                return d < 320.0f && local[2] > 10.0f && std::fabs(local[0]) < local[2] * 0.6f;
            case grenade: {
                // Thrown at 75 ft/s, half a radian up, falling at 2.5 g: about 60 ft on level ground, more at speed.
                float speed = dot(me.vel, &me.m[6]);
                float reach = 60.0f + std::max(0.0f, speed) * 0.9f;
                return d > 15.0f && d < reach && std::fabs(angle) < window * 2.0f;
            }
            case sonic:
                return d < 55.0f;
            case mine:
                // Behind and close: a chaser.
                return local[2] < -6.0f && local[2] > -90.0f && std::fabs(local[0]) < 18.0f;
            default:
                return false;
        }
    }

    void decide(const Nav* nav, Brain& b, const SkillInfo& skill, const rush2::battle_ai::World& w, int i) {
        const auto& me = w.cars[i];
        float health = me.health / max_health;
        Goal best_goal = Goal::roam;
        int best_index = -1;
        float best = 0.0f;
        int my_node = nav != nullptr ? nav->locate(me.pos) : -1;
        auto reachable = [&](const float p[3]) {
            if (nav == nullptr || my_node < 0) return true;
            int n = nav->locate(p);
            return n >= 0 && nav->component[n] == nav->component[my_node];
        };
        // Enemy cars.
        for (int j = 0; j < max_cars; j++) {
            const auto& o = w.cars[j];
            if (!o.alive || !enemies(w, i, j) || b.target_unreachable[j] > 0.0f) continue;
            float d = distance(me.pos, o.pos);
            if (o.invisible && d > 30.0f) continue;
            if (!reachable(o.pos)) continue;
            float score = 70.0f + b.p.aggression - d * 0.15f + (1.0f - o.health / max_health) * 25.0f;
            if (j == me.last_attacker) score += 20.0f;
            if (b.goal == Goal::attack && b.target == j) score += 15.0f;
            if (me.weapon == gun) score -= 15.0f;
            if (me.weapon == mine) score -= 45.0f;
            // Spread out: a car other opponents are already after, or one in a crowd, is worth less.
            for (int k = 0; k < max_cars; k++) {
                if (k == i || k == j || !w.cars[k].alive) continue;
                if (w.cars[k].bot && brains[k].goal == Goal::attack && brains[k].target == j) score -= 14.0f;
                if (distance(w.cars[k].pos, o.pos) < 30.0f) score -= 8.0f;
            }
            // Hurt: keep away from a car in better shape.
            if (health < b.p.caution && o.health > me.health) score -= 35.0f;
            // Just got out of a pile: leave the cars alone for a moment.
            if (b.disengage > 0.0f) score -= 60.0f;
            if (score > best) {
                best = score;
                best_goal = Goal::attack;
                best_index = j;
            }
        }
        // Pickups.
        if (b.unreachable.size() != w.pickups.size()) b.unreachable.assign(w.pickups.size(), 0.0f);
        for (size_t k = 0; k < w.pickups.size(); k++) {
            const auto& p = w.pickups[k];
            if (!p.available || b.unreachable[k] > 0.0f) continue;
            float d = distance(me.pos, p.pos);
            if (d > skill.reach || !reachable(p.pos)) continue;
            float score;
            if (p.kind < 8) {
                if (me.weapon == gun) score = 85.0f - d * 0.2f;
                else if (me.weapon == p.kind) score = 25.0f - d * 0.3f;
                else score = 12.0f - d * 0.3f;
                if (p.kind == mine || p.kind == ram) score -= 15.0f;
            }
            else if (p.kind == kind_heal) {
                score = (1.0f - health) * 120.0f - 10.0f - d * 0.2f;
            }
            else {
                score = 40.0f + (1.0f - health) * 40.0f - d * 0.2f;
            }
            score += b.p.greed;
            // Another car closer to it will likely get there first; another opponent already going for it closer
            // than this one will.
            for (int j = 0; j < max_cars; j++) {
                if (j == i || !w.cars[j].alive) continue;
                float dj = distance(w.cars[j].pos, p.pos);
                if (dj < d * 0.5f) score -= 25.0f;
                if (w.cars[j].bot && brains[j].goal == Goal::pickup && brains[j].pickup == (int)k && dj < d) score -= 40.0f;
            }
            if (b.goal == Goal::pickup && b.pickup == (int)k) score += 15.0f;
            if (score > best) {
                best = score;
                best_goal = Goal::pickup;
                best_index = (int)k;
            }
        }
        if (best_goal != b.goal || (best_goal == Goal::attack && best_index != b.target) ||
            (best_goal == Goal::pickup && best_index != b.pickup)) {
            b.path.clear();
        }
        if (best_goal == Goal::roam && (b.goal != Goal::roam || flat_distance(me.pos, b.roam_to) < 20.0f)) {
            // Somewhere else in the arena it can get to, away from the other cars (and the places other opponents
            // roam to): the best of a few random open nodes of its own component.
            b.roam_to[0] = me.pos[0];
            b.roam_to[1] = me.pos[1];
            b.roam_to[2] = me.pos[2];
            if (nav != nullptr && my_node >= 0) {
                float best_roam = -1.0f;
                for (int tries = 0, found = 0; tries < 128 && found < 12; tries++) {
                    int n = std::uniform_int_distribution<int>(0, nav->nodes() - 1)(rng);
                    if (nav->component[n] != nav->component[my_node] || nav->wall_distance[n] < 2) continue;
                    float p[3];
                    nav->node_pos(n, p);
                    if (flat_distance(p, me.pos) < 80.0f) continue;
                    found++;
                    float apart = 200.0f;
                    for (int j = 0; j < max_cars; j++) {
                        if (j == i || !w.cars[j].alive) continue;
                        apart = std::min(apart, flat_distance(p, w.cars[j].pos));
                        if (w.cars[j].bot && brains[j].goal == Goal::roam) apart = std::min(apart, flat_distance(p, brains[j].roam_to));
                    }
                    float score = apart + 60.0f * random_unit();
                    if (score > best_roam) {
                        best_roam = score;
                        memcpy(b.roam_to, p, sizeof(p));
                    }
                }
            }
            b.path.clear();
        }
        b.goal = best_goal;
        b.target = best_goal == Goal::attack ? best_index : -1;
        b.pickup = best_goal == Goal::pickup ? best_index : -1;
    }

    // One tick of computer car i: what it does and the inputs for its step.
    void run(uint8_t* rdram, const Nav* nav, Brain& b, const SkillInfo& skill, const rush2::battle_ai::World& w, int i) {
        const auto& me = w.cars[i];
        float dt = w.dt;
        for (float& t : b.unreachable) t = std::max(0.0f, t - dt);
        for (float& t : b.target_unreachable) t = std::max(0.0f, t - dt);
        b.pinned = std::max(0.0f, b.pinned - dt * 0.25f);
        b.disengage = std::max(0.0f, b.disengage - dt);
        b.side_time -= dt;
        if (b.side_time <= 0.0f) {
            b.side = random_unit() < 0.5f ? -1.0f : 1.0f;
            b.side_time = 4.0f + 5.0f * random_unit();
        }
        b.buttons = 0;
        // The car may drive: the race is on (a drone's +0x71C is already set during the start countdown) and it isn't
        // waiting after a respawn.
        bool released = MEM_W(0, (int32_t)game_state) == 3 && MEM_H(0, (int32_t)(car_at(i) + car_release)) != 0;
        if (!me.alive || w.over) {
            b.steer = b.throttle = 0.0f;
            b.brake = 1.0f;
            b.gear = 1;
            b.path.clear();
            b.stuck = b.reverse = b.idle = 0.0f;
            memcpy(b.idle_from, me.pos, sizeof(b.idle_from));
            b.held = 0;
            return;
        }
        b.think -= dt;
        if (b.think <= 0.0f || (b.goal == Goal::attack && !w.cars[b.target].alive) ||
            (b.goal == Goal::pickup && (b.pickup >= (int)w.pickups.size() || !w.pickups[b.pickup].available))) {
            decide(nav, b, skill, w, i);
            b.think = skill.think * (0.8f + 0.4f * random_unit());
        }
        std::vector<std::array<float, 3>> avoid;
        for (const auto& t : w.threats) {
            if (t.mine && t.owner != i) avoid.push_back({ t.pos[0], t.pos[1], t.pos[2] });
        }

        // Where to.
        float dest[3] = { b.roam_to[0], b.roam_to[1], b.roam_to[2] };
        bool chase = false;
        if (b.goal == Goal::attack) {
            const auto& t = w.cars[b.target];
            float d = distance(me.pos, t.pos);
            lead(t, std::min(d / 200.0f, 0.6f), dest);
            chase = true;
            // Close in, a weapon that needn't point straight at the target (the guns turn up to about 26 degrees,
            // missiles home, grenades arc) heads for a point beside it: the car circles it rather than queueing
            // behind it with the others.
            bool beside = me.weapon == gun || me.weapon == gatling || me.weapon == missile || me.weapon == grenade;
            float flat = flat_distance(me.pos, dest);
            if (beside && flat < b.p.range * 1.8f && flat > 1.0f) {
                float offset = std::min(b.p.range * 0.4f, 16.0f) * b.side;
                float across[3] = { (dest[2] - me.pos[2]) / flat, 0.0f, -(dest[0] - me.pos[0]) / flat };
                float moved[3] = { dest[0] + across[0] * offset, dest[1], dest[2] + across[2] * offset };
                // Not into a wall: the other side, or straight at it.
                if (nav == nullptr || nav->straight(t.pos, moved)) {
                    memcpy(dest, moved, sizeof(dest));
                }
                else {
                    float other[3] = { dest[0] - across[0] * offset, dest[1], dest[2] - across[2] * offset };
                    if (nav->straight(t.pos, other)) {
                        memcpy(dest, other, sizeof(dest));
                        b.side = -b.side;
                    }
                }
            }
        }
        else if (b.goal == Goal::pickup) {
            memcpy(dest, w.pickups[b.pickup].pos, sizeof(dest));
        }
        float point[3];
        if (!steer_point(nav, b, me, dest, avoid, dt, point)) {
            // No way there: try something else for a while.
            if (b.goal == Goal::pickup) b.unreachable[b.pickup] = 10.0f;
            if (b.goal == Goal::attack) b.target_unreachable[b.target] = 5.0f;
            b.think = 0.0f;
        }
        float rel[3] = { point[0] - me.pos[0], point[1] - me.pos[1], point[2] - me.pos[2] };
        float local[3];
        to_local(me.m, rel, local);
        float angle = std::atan2(local[0], local[2]);
        float forward_speed = dot(me.vel, &me.m[6]);
        float dest_distance = flat_distance(me.pos, dest);

        // A missile coming: swerve across its path.
        if (skill.dodge && b.evade <= 0.0f) {
            for (const auto& t : w.threats) {
                if (!t.missile || t.owner == i) continue;
                float to[3] = { me.pos[0] - t.pos[0], me.pos[1] - t.pos[1], me.pos[2] - t.pos[2] };
                float d = length(to);
                if (d > 140.0f || d < 1.0f || dot(to, t.forward) < d * 0.85f) continue;
                float side[3];
                to_local(me.m, t.forward, side);
                b.evade = 0.7f;
                b.evade_turn = side[0] > 0.0f ? -1.0f : 1.0f;
                break;
            }
        }

        float steer = std::clamp(angle * 1.8f, -1.0f, 1.0f);
        float want = top_speed * skill.speed * b.p.pace * std::clamp(1.0f - std::fabs(angle) / 1.3f, 0.25f, 1.0f);
        // Shooting: hold its range back at the target's speed rather than run into it (the ram runs into it).
        // A car turns only while it moves: one that isn't facing its target yet keeps going round at a turning speed.
        if (chase && me.weapon != ram) {
            const auto& t = w.cars[b.target];
            float target_distance = flat_distance(me.pos, t.pos);
            if (target_distance < b.p.range + 25.0f) {
                float target_speed = std::hypot(t.vel[0], t.vel[2]);
                want = std::fabs(angle) < 0.5f
                           ? std::min(want, std::max(0.0f, target_speed + (target_distance - b.p.range) * 1.5f))
                           : std::min(want, 35.0f);
            }
        }
        // Other cars close ahead (not a target it rams): steer round them rather than pile into them.
        if (b.evade <= 0.0f) {
            float push = 0.0f;
            for (int j = 0; j < max_cars; j++) {
                if (j == i || !w.cars[j].present || !w.cars[j].alive) continue;
                if (chase && j == b.target && me.weapon == ram) continue;
                float to[3] = { w.cars[j].pos[0] - me.pos[0], 0.0f, w.cars[j].pos[2] - me.pos[2] };
                float l[3];
                to_local(me.m, to, l);
                float d = std::hypot(l[0], l[2]);
                if (d > 30.0f || l[2] < 0.0f || std::fabs(l[0]) > 12.0f) continue;
                push += (l[0] >= 0.0f ? -1.0f : 1.0f) * (1.0f - d / 30.0f);
            }
            steer = std::clamp(steer + push * 0.9f, -1.0f, 1.0f);
        }
        if (b.goal == Goal::pickup && dest_distance < 30.0f) want = std::min(want, 45.0f + dest_distance);
        float throttle = std::clamp((want - forward_speed) / 20.0f + 0.3f, 0.0f, 1.0f);
        float brake = forward_speed > want + 20.0f ? std::clamp((forward_speed - want - 20.0f) / 30.0f, 0.0f, 1.0f) : 0.0f;
        if (brake > 0.0f) throttle = 0.0f;
        int gear = 1;
        if (b.evade > 0.0f) {
            b.evade -= dt;
            steer = b.evade_turn;
            throttle = 1.0f;
            brake = 0.0f;
        }

        // Stopped while trying to go, or the way is behind and close: back out with opposite lock.
        if (released && b.reverse <= 0.0f) {
            if (throttle > 0.5f && std::fabs(forward_speed) < 4.0f) b.stuck += dt;
            else b.stuck = std::max(0.0f, b.stuck - dt);
            // (Not after a car: it moves, and turning round at speed keeps the car in the fight.)
            bool behind = !chase && std::fabs(angle) > 2.0f && forward_speed < 25.0f && dest_distance < 40.0f;
            if (b.stuck > 0.8f || behind) {
                b.reverse = behind ? 0.8f : 1.1f;
                b.reverse_steer = angle > 0.0f ? -1.0f : 1.0f;
                b.stuck = 0.0f;
                // Stuck again and again (in a pile against a wall): back off from the cars for a while.
                if (!behind) {
                    b.pinned += 1.0f;
                    if (b.pinned >= 2.0f) {
                        b.pinned = 0.0f;
                        b.disengage = 3.5f;
                        b.think = 0.0f;
                        if (b.goal == Goal::attack) b.target_unreachable[b.target] = 3.5f;
                    }
                }
            }
        }
        if (b.reverse > 0.0f) {
            b.reverse -= dt;
            gear = -1;
            throttle = 1.0f;
            brake = 0.0f;
            steer = b.reverse_steer;
        }
        // Getting nowhere for long (on its roof, wedged): wrecked, and respawned by the game.
        if (released) {
            b.idle += dt;
            if (distance(me.pos, b.idle_from) > 15.0f) {
                b.idle = 0.0f;
                memcpy(b.idle_from, me.pos, sizeof(b.idle_from));
            }
            bool upside_down = me.m[4] < 0.2f;
            if (b.idle > (upside_down ? 3.0f : 10.0f)) {
                MEM_B(0, (int32_t)(car_at(i) + car_wrecked)) = 1;
                b.idle = 0.0f;
            }
        }
        b.steer = steer;
        b.throttle = throttle;
        b.brake = brake;
        b.gear = gear;

        // Weapons: at the car it is after, else at any enemy the weapon could hit now.
        if (released) {
            int shoot = -1;
            if (b.goal == Goal::attack && should_fire(nav, skill, w, i, b.target)) shoot = b.target;
            for (int j = 0; j < max_cars && shoot < 0; j++) {
                if (w.cars[j].alive && enemies(w, i, j) && !(w.cars[j].invisible && distance(me.pos, w.cars[j].pos) > 30.0f) &&
                    should_fire(nav, skill, w, i, j)) {
                    shoot = j;
                }
            }
            uint8_t want_buttons = 0;
            if (shoot >= 0 && me.weapon != ram && me.cooldown <= 0.0f && random_unit() < skill.fire_chance) {
                want_buttons = rush2::controls::battle_fire;
            }
            if (me.weapon == gatling && shoot >= 0) want_buttons = rush2::controls::battle_fire;
            // A press is a change: let go a tick between shots (the gatling fires while held).
            if (me.weapon != gatling && (b.held & rush2::controls::battle_fire) != 0) want_buttons &= ~rush2::controls::battle_fire;
            b.buttons = want_buttons;
            // Test aid: R2_BATTLE_TEST=nofire (driving only).
            if (test_flag("nofire")) b.buttons = 0;
        }
        b.held = b.buttons;

        if (test_flag("ai") && std::fmod(ai_clock + (float)i * 0.13f, 1.0f) < dt) {
            static const char* names[] = { "none", "attack", "pickup", "roam" };
            fprintf(stderr, "[BattleAI] car %d %s t=%d p=%d dest %.0f %.0f d %.0f want %.0f pos %.0f %.0f %.0f v %.0f ang %.2f steer %.2f thr %.2f brk %.2f gear %d "
                            "weapon %d hp %.0f path %d/%d fire %d wreck %d respawn %d release %d\n",
                    i, names[(int)b.goal], b.target, b.pickup, dest[0], dest[2], dest_distance, want, me.pos[0], me.pos[1], me.pos[2], forward_speed, angle, steer,
                    throttle, brake, gear, me.weapon, me.health, (int)b.path_at, (int)(b.path.size() / 3), b.buttons,
                    MEM_B(0, (int32_t)(car_at(i) + car_wrecked)), MEM_B(0, (int32_t)(car_state_at(i) + 0x343)),
                    (int)(int16_t)MEM_H(0, (int32_t)(car_at(i) + car_release)));
        }
    }

    // Calls a game function from a hook, keeping the hooked function's registers (as src/ghost.cpp does).
    void call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0, int32_t a1 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        func(rdram, ctx);
        *ctx = saved;
    }
}

// ----------------------------------------------------------------------------------------------------------------
// Interface

int rush2::battle_ai::race_opponents(int wanted, int players) {
    // Test aid: R2_BATTLE_TEST=bots<n> races n opponents whatever DRONES says.
    if (const char* test = getenv("R2_BATTLE_TEST"); test != nullptr && strstr(test, "bots") != nullptr) {
        wanted = atoi(strstr(test, "bots") + 4);
    }
    return std::clamp(wanted, 0, std::max(0, max_cars - players));
}

void rush2::battle_ai::set_arena(const std::vector<float>& triangles) {
    int generation = ++nav_generation;
    {
        std::lock_guard lock{ nav_mutex };
        nav_ready = nullptr;
    }
    std::thread([triangles, generation]() {
        auto nav = build_nav(triangles);
        std::lock_guard lock{ nav_mutex };
        if (generation == nav_generation) nav_ready = nav;
    }).detach();
}

void rush2::battle_ai::begin_round() {
    std::lock_guard lock{ ai_mutex };
    rng.seed(std::random_device{}());
    for (Brain& b : brains) {
        b = Brain{};
        b.p.aggression = -15.0f + 30.0f * random_unit();
        b.p.greed = -15.0f + 30.0f * random_unit();
        b.p.range = 25.0f + 30.0f * random_unit();
        b.p.caution = 0.2f + 0.3f * random_unit();
        b.p.pace = 0.9f + 0.1f * random_unit();
        b.side = random_unit() < 0.5f ? -1.0f : 1.0f;
    }
    ai_clock = 0.0f;
    round_live = true;
}

void rush2::battle_ai::update(uint8_t* rdram, const World& world) {
    std::lock_guard lock{ ai_mutex };
    auto nav = current_nav();
    const SkillInfo skill = skill_at(MEM_BU(0, (int32_t)race_difficulty));
    ai_clock += world.dt;
    for (int i = 0; i < max_cars; i++) {
        if (!world.cars[i].present || !world.cars[i].bot) continue;
        Brain& b = brains[i];
        run(rdram, nav.get(), b, skill, world, i);
        uint32_t car = car_at(i);
        write_f(rdram, car + car_steer, b.steer);
        write_f(rdram, car + car_throttle, b.throttle);
        write_f(rdram, car + car_brake, b.brake);
        MEM_H(0, (int32_t)(car + car_gear)) = (int16_t)b.gear;
    }
}

uint8_t rush2::battle_ai::buttons(int car) {
    std::lock_guard lock{ ai_mutex };
    return car >= 0 && car < max_cars ? brains[car].buttons : 0;
}

bool rush2::battle_ai::is_bot(uint8_t* rdram, int car) {
    return bot_car(rdram, car);
}

int rush2::battle_ai::bot_number(uint8_t* rdram, int car) {
    if (!bot_car(rdram, car)) return 0;
    int n = 0;
    for (int i = 0; i <= car; i++) n += bot_car(rdram, i) ? 1 : 0;
    return n;
}

// Start of func_80074990 (a drone's driver, $a0 = the car): a computer opponent's inputs are this file's. Returns 1 to
// skip the driver.
extern "C" int rush2_battle_ai_drive(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    if (car < cars || car >= cars + max_cars * car_size) return 0;
    int i = (int)((car - cars) / car_size);
    // In its step (rush2_battle_ai_tick_end) the car is a human's.
    return stepping[i] || bot_car(rdram, i) ? 1 : 0;
}

// Start of func_80075880 (a car's physics tick, $a0 = the car index): a computer opponent's scheduled step (drones
// are stepped round robin) is skipped; rush2_battle_ai_tick_end steps it every tick. Returns 1 to skip the tick.
extern "C" int rush2_battle_ai_car_tick(uint8_t* rdram, recomp_context* ctx) {
    int car = (int32_t)ctx->r4;
    return car >= 0 && car < max_cars && !stepping[car] && bot_car(rdram, car) ? 1 : 0;
}

// func_80076578 at 0x8007660C, after the players' and drones' steps of the physics tick: each computer opponent's step,
// as func_80075C3C steps a drone (func_80075880 with the clock's bits, then func_800663CC if 0x800D042C).
extern "C" void rush2_battle_ai_tick_end(uint8_t* rdram, recomp_context* ctx) {
    for (int car = 0; car < max_cars; car++) {
        if (!bot_car(rdram, car)) continue;
        float t = read_f(rdram, physics_clock);
        int32_t bits;
        memcpy(&bits, &t, 4);
        // Its step is a player's car's, so it fights the players on their terms: the physics (func_80071D78 and the
        // functions it calls) treats a human's car (+0x7E8 == 2) differently from a drone's (a crash wrecks a drone
        // always, a human's car as the cheats allow, func_800715E8), so the car is one for the step, as a ghost is
        // (src/ghost.cpp); its player record (car state +0x350) stays 0. Its clutch is a player's too, from its rpm
        // and throttle (func_800806A4; a drone's stays at the race start's 0).
        int32_t kind = (int32_t)(car_at(car) + car_kind);
        int8_t own_kind = MEM_B(0, kind);
        MEM_B(0, kind) = kind_human;
        call(rdram, ctx, player_join_init_car_dynamics_800806A4, (int32_t)car_at(car));
        stepping[car] = true;
        call(rdram, ctx, physics_car_tick_80075880, car, bits);
        stepping[car] = false;
        MEM_B(0, kind) = own_kind;
        if (MEM_BU(0, (int32_t)drone_extra) != 0) call(rdram, ctx, car_material_effects_update_800663CC, car);
    }
}
