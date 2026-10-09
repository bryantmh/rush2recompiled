// SF Rush track 6's traffic buses (docs/rush1_research.md, "Track 6 traffic buses").
//
// Rush 1 drives eight BUSO1 buses round two loops of track 6 (func_800A9DB8 sets them up, func_8007FC74 moves them
// each physics tick). Rush 2's subway trains are the same system grown up: subway_init (func_800A4210) and
// subway_update (func_80075FB8) are Rush 1's two functions with a train-car link added, and car_collide_with_cars
// routes cars into the trains' pseudo-car bodies (func_8006EC78) as Rush 1 does into the buses' (func_80079528).
// Rush 2's subway state array holds only 6 entries, so on SF Rush track 6 the two subway functions are replaced by
// this port of Rush 1's, which keeps the bus states here and their poses (matrix + position, what each scene node
// points at) in the game heap next to the pseudo-car bodies. Rush 2's car collision then hits the buses as it hits the
// trains, with the bus's own reach and height (Rush 1's 25 and 12.5 for the trains' 72 and 18). Rush 1's computer
// drivers also lift off behind a bus (func_8007CF74), ported into Rush 2's driver at the same point.
//
// Rush 1 positions the buses in render space, which is Rush 2's world space (docs/rush1_research.md §5), so the paths
// and poses carry over unchanged.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"

extern "C" void heap_alloc_80083A0C(uint8_t* rdram, recomp_context* ctx);
extern "C" void model_find_by_name_8005BE3C(uint8_t* rdram, recomp_context* ctx);
extern "C" void scene_node_create_8007F6DC(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr int bus_track = 6;                    // Rush 1 track (1-7) with the buses.
    constexpr int bus_count = 8;
    constexpr uint32_t r1_bus_records = 0x800C6848; // Rush 1: 8 x 0x14 {path, start position f32[3], s8 start node}.
    constexpr uint32_t r1_record_size = 0x14;
    constexpr const char* bus_model = "BUSO1";

    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t game_state = 0x8010C0D0;     // 2 = attract mode.
    constexpr uint32_t model_container_count = 0x800D5788;
    constexpr uint32_t subway_count = 0x800D4E74;   // s8: number of subway pseudo-cars.
    constexpr uint32_t subway_bodies = 0x800D50E4;  // Pointer to the 0x81C-byte pseudo-cars.
    constexpr uint32_t body_size = 0x81C;
    constexpr uint32_t pose_size = 0x30;            // f32[9] matrix + f32[3] position.
    constexpr uint32_t wreck_threshold = 0x800CFF4C;
    constexpr uint32_t car_table = 0x800C08B0;      // +0x58: the trains' collision mass.

    // Rush 1's turn limits (func_8007FC74, 0x800D78E0-0x800D7908).
    constexpr float pi = 3.14159274f;
    constexpr float two_pi = 6.28318548f;
    constexpr float max_turn = 0.785398185f;        // Per second, towards the node's heading.
    constexpr float max_turn_change = 0.196349546f; // Per tick.
    constexpr float far = 1.0e7f;

    struct Node {
        float x, z;
        int16_t speed;   // < 0 ends the path (back to node 0).
        float heading;
    };
    struct Bus {
        int path = 0;
        int node = 0;
        float speed = 0, dist = far, heading = 0, turn = 0;
        float pos[3] = {};
        float matrix[9] = {};
    };

    std::vector<Node> paths[2];
    uint32_t path_addrs[2] = {};
    std::array<Bus, bus_count> buses;
    bool active = false;

    float rf(uint8_t* rdram, uint32_t a) {
        uint32_t w = MEM_W(0, (int32_t)a);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }
    void wf(uint8_t* rdram, uint32_t a, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)a) = w;
    }

    // Calls a game function from a hook, keeping the hooked function's registers, with an optional fifth (stack)
    // argument. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0, int32_t a4 = 0) {
        recomp_context saved = *ctx;
        ctx->r29 = (int32_t)ctx->r29 - 0x20;
        MEM_W(0x10, (int32_t)ctx->r29) = (uint32_t)a4;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    bool load_paths(std::string& error) {
        auto rom = rush2::track1::get_rom();
        auto main = rom ? rush2::track1::main_code(*rom) : nullptr;
        if (!main) {
            error = "no SF Rush main code";
            return false;
        }
        auto at = [&](uint32_t vram) -> const uint8_t* {
            uint32_t o = vram - rush2::track1::main_vram;
            return o + 16 <= main->size() ? main->data() + o : nullptr;
        };
        auto u32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        auto f32 = [&](const uint8_t* p) { uint32_t w = u32(p); float f; memcpy(&f, &w, 4); return f; };
        for (auto& p : paths) p.clear();
        int n_paths = 0;
        for (int i = 0; i < bus_count; i++) {
            const uint8_t* r = at(r1_bus_records + i * r1_record_size);
            if (r == nullptr) {
                error = "bus records out of range";
                return false;
            }
            uint32_t addr = u32(r);
            int k = 0;
            while (k < n_paths && path_addrs[k] != addr) k++;
            if (k == n_paths) {
                if (n_paths == 2) {
                    error = "more than two bus paths";
                    return false;
                }
                path_addrs[n_paths++] = addr;
                for (int j = 0;; j++) {
                    const uint8_t* q = at(addr + j * 0x10);
                    if (q == nullptr || j > 256) {
                        error = "bus path out of range";
                        return false;
                    }
                    Node node{ f32(q), f32(q + 4), int16_t(q[8] << 8 | q[9]), f32(q + 12) };
                    paths[k].push_back(node);
                    if (node.speed < 0) break;
                }
            }
            Bus& b = buses[i];
            b = Bus{};
            b.path = k;
            b.node = int8_t(r[0x10]);
            for (int c = 0; c < 3; c++) b.pos[c] = f32(r + 4 + c * 4);
        }
        return true;
    }

    // func_8007FC14: the bus's yaw matrix for angle a.
    void yaw_matrix(float a, float m[9]) {
        float c = cosf(a), s = sinf(a);
        m[0] = c; m[1] = 0; m[2] = s;
        m[3] = 0; m[4] = 1; m[5] = 0;
        m[6] = -s; m[7] = 0; m[8] = c;
    }

    void write_pose(uint8_t* rdram, uint32_t pose, const Bus& b) {
        for (int i = 0; i < 9; i++) wf(rdram, pose + i * 4, b.matrix[i]);
        for (int i = 0; i < 3; i++) wf(rdram, pose + 0x24 + i * 4, b.pos[i]);
    }

    // func_80075EAC (Rush 1 func_8007FB30) on the bus: the pseudo-car's velocity, position a little ahead of the
    // bus, and orientation, which the car collision reads.
    void update_body(uint8_t* rdram, uint32_t body, const Bus& b, float step) {
        const float* fwd = &b.matrix[6];
        for (int i = 0; i < 3; i++) {
            wf(rdram, body + 0x7A4 + i * 4, fwd[i] * b.speed);
            wf(rdram, body + 0x7B0 + i * 4, fwd[i] * (2.0f * step) + b.pos[i]);
        }
        for (int i = 0; i < 9; i++) {
            wf(rdram, body + 0x2CC + i * 4, b.matrix[i]);
            wf(rdram, body + 0x7BC + i * 4, b.matrix[i]);
        }
    }

    uint32_t pose_of(uint8_t* rdram, int i) {
        return MEM_W(0, (int32_t)subway_bodies) + bus_count * body_size + i * pose_size;
    }
}

// Start of subway_init (func_800A4210, $a0 = whether the heap block is new): on SF Rush track 6, Rush 1's
// func_800A9DB8 instead. Returns true when it handled the call.
extern "C" int rush2_track1_buses_init(uint8_t* rdram, recomp_context* ctx) {
    active = false;
    if (rush2::track1::race_track() != bus_track || MEM_B(0, (int32_t)track_id) != rush2::track2049::host_slot) {
        return 0;
    }
    std::string error;
    if (!load_paths(error)) {
        fprintf(stderr, "[track1] No buses: %s\n", error.c_str());
        MEM_B(0, (int32_t)subway_count) = 0;
        MEM_W(0, (int32_t)subway_bodies) = 0;
        return 1;
    }
    int containers = MEM_BU(0, (int32_t)model_container_count);
    // The model name goes on the stack below the callee's argument area.
    uint32_t name = (uint32_t)ctx->r29 - 0x40;
    for (size_t i = 0; i <= strlen(bus_model); i++) MEM_B(i, (int32_t)name) = bus_model[i];
    ctx->r29 = (int32_t)ctx->r29 - 0x40;
    int model = call(rdram, ctx, model_find_by_name_8005BE3C, (int32_t)name, 0, containers - 1, 1);
    ctx->r29 = (int32_t)ctx->r29 + 0x40;
    if (model < 0) {
        fprintf(stderr, "[track1] No buses: model %s missing\n", bus_model);
        MEM_B(0, (int32_t)subway_count) = 0;
        MEM_W(0, (int32_t)subway_bodies) = 0;
        return 1;
    }
    // Like Rush 2's trains, the block is allocated when the heap is new and reused for a restart.
    if (ctx->r4 != 0 || MEM_W(0, (int32_t)subway_bodies) == 0) {
        MEM_W(0, (int32_t)subway_bodies) =
            (uint32_t)call(rdram, ctx, heap_alloc_80083A0C, bus_count * (body_size + pose_size), 0);
    }
    MEM_B(0, (int32_t)subway_count) = bus_count;
    int mode = MEM_W(0, (int32_t)game_state) == 2 ? 1 : 3;
    for (int i = 0; i < bus_count; i++) {
        Bus& b = buses[i];
        const Node& n = paths[b.path][b.node];
        // Rush 1 builds the first matrix from the node's heading as stored; the moves use its negation.
        b.heading = n.heading;
        yaw_matrix(b.heading, b.matrix);
        b.dist = far;
        b.speed = n.speed;
        b.turn = 0;
        uint32_t pose = pose_of(rdram, i);
        write_pose(rdram, pose, b);
        call(rdram, ctx, scene_node_create_8007F6DC, model, (int32_t)pose, mode, -1, 0);
    }
    float threshold = rf(rdram, wreck_threshold), mass = rf(rdram, car_table + 0x58);
    // The bus's box (Rush 1: 20 long, 6 wide, 12 up, in Rush 1's car axes) in Rush 2's car axes (x side, y up,
    // z forward), corner by corner as Rush 2 lays out the trains'.
    const float corners[4][3] = { { 6, 12, 20 }, { -6, 12, 20 }, { 6, 12, -20 }, { -6, 12, -20 } };
    uint32_t bodies = MEM_W(0, (int32_t)subway_bodies);
    for (int i = 0; i < bus_count; i++) {
        uint32_t body = bodies + i * body_size;
        MEM_H(0x7E0, (int32_t)body) = -1;
        MEM_B(0x7EA, (int32_t)body) = 0;
        wf(rdram, body + 0x644, threshold);
        wf(rdram, body + 0x5B4, mass);
        for (int c = 0; c < 4; c++) {
            for (int k = 0; k < 3; k++) wf(rdram, body + 0xE8 + c * 12 + k * 4, corners[c][k]);
        }
        update_body(rdram, body, buses[i], 0.0f);
    }
    active = true;
    fprintf(stderr, "[track1] %d buses on track 6\n", bus_count);
    fflush(stderr);
    return 1;
}

// Start of subway_update (func_80075FB8, $f12 = the tick's dt): on SF Rush track 6, Rush 1's func_8007FC74
// instead. Returns true when it handled the call.
extern "C" int rush2_track1_buses_update(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        return 0;
    }
    if (rush2::track1::race_track() != bus_track || MEM_B(0, (int32_t)subway_count) != bus_count ||
        MEM_W(0, (int32_t)subway_bodies) == 0) {
        active = false;
        return 0;
    }
    float dt = ctx->f12.fl;
    uint32_t bodies = MEM_W(0, (int32_t)subway_bodies);
    for (int i = 0; i < bus_count; i++) {
        Bus& b = buses[i];
        const std::vector<Node>& path = paths[b.path];
        const Node& n = path[b.node];
        float step = b.speed * dt;
        float dx = b.pos[0] - n.x, dz = b.pos[2] - n.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < step || b.dist < d) {
            // Reached (or passed) the node: take its speed and head for the next.
            b.speed = n.speed;
            b.node++;
            b.dist = far;
            if (path[b.node].speed < 0) b.node = 0;
        }
        else {
            b.dist = d;
            float want = n.heading - b.heading;
            if (want > pi) want -= two_pi;
            else if (want < -pi) want += two_pi;
            b.speed = b.speed + ((float)n.speed - b.speed) * step / d;
            if (want > max_turn) want = max_turn;
            else if (want < -max_turn) want = -max_turn;
            float turn = want * dt;
            float change = turn - b.turn;
            if (change > max_turn_change) turn = b.turn + max_turn_change;
            else if (change < -max_turn_change) turn = b.turn - max_turn_change;
            b.turn = turn;
            b.heading += turn;
            if (b.heading > pi) b.heading -= two_pi;
            else if (b.heading < -pi) b.heading += two_pi;
            yaw_matrix(-b.heading, b.matrix);
        }
        for (int k = 0; k < 3; k++) b.pos[k] += step * b.matrix[6 + k];
        write_pose(rdram, pose_of(rdram, i), b);
        update_body(rdram, bodies + i * body_size, b, step);
    }
    return 1;
}

// func_8006EC78 at 0x8006ED1C, after it loads the trains' reach ($f24 = 72, added to the car's radius) and height
// ($f20 = 18): the buses' are Rush 1's (func_80079528: 25 and 12.5).
extern "C" void rush2_track1_buses_collision(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        return;
    }
    ctx->f24.fl = 25.0f;
    ctx->f20.fl = 12.5f;
}

// func_80071FBC at 0x80072564, after its loop over the other cars has lowered the throttle limit ($sp+0x134) for the
// cars ahead ($s5 = the driver's car + 0x7BC, its orientation; +0x7B0 its position). Rush 1 (func_8007CF74) also
// limits it for a bus up to 200 ahead and within 14 to the side: 0.8 within 120, rising to 1 at 200.
extern "C" void rush2_track1_buses_drive(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        return;
    }
    uint32_t m = (uint32_t)ctx->r21, car = m - 0x7BC, limit = (uint32_t)ctx->r29 + 0x134;
    float mat[9], own[3];
    for (int i = 0; i < 9; i++) mat[i] = rf(rdram, m + i * 4);
    for (int i = 0; i < 3; i++) own[i] = rf(rdram, car + 0x7B0 + i * 4);
    float lim = rf(rdram, limit);
    for (const Bus& b : buses) {
        float d[3] = { b.pos[0] - own[0], b.pos[1] - own[1], b.pos[2] - own[2] };
        float side = mat[0] * d[0] + mat[1] * d[1] + mat[2] * d[2];
        float ahead = mat[6] * d[0] + mat[7] * d[1] + mat[8] * d[2];
        if (ahead < 0.0f || ahead > 200.0f || fabsf(side) > 14.0f) continue;
        float f = ahead < 120.0f ? 0.8f : 1.0f - (200.0f - ahead) * 0.0025f;
        if (f < lim) lim = f;
    }
    wf(rdram, limit, lim);
}
