// California Speed's wall collision, run in place of Rush 2's body probe on its tracks
// (docs/calspeed_research.md#walls-california-speeds-body-collision).
//
// Rush 2's body probe (car_wall_collision_query_8006FC94) tests each of the car's four body points on its own: a point
// that projects inside a wall polygon and lies from 5 ft behind its plane to 3.75 ft in front gets the wall response.
// California Speed does not use it. Its ground contact (func_800A58A4) calls func_800B1624, which gathers the leaves
// under the four body points, and for every wall polygon in them (surface type 4 and up) runs func_800B0E80: the four
// edges of the body quad, in the polygon's frame, are each cut where they cross the polygon's plane, the part behind
// the plane is clipped to the polygon's footprint, and the deepest clipped edge is the hit; its depth along the normal,
// at least 0.0005 ft, is the hit's depth. A wall a body point merely lies near, in front of or behind, is no hit, so
// the ceilings 2-4 ft under San Francisco's street and the sloped faces over its subway ramps, which Rush 2's window
// took, are left alone. func_800B0A04 applies a hit: the car is moved out along the normal by the depth (position, its
// copy, body points, wheel contact points), and the hit body point gets a force against its velocity in the wall's
// frame (0.006 x 10000 / mass x 1/dt along the wall, 0.1 of that into it, plus 8/dt x (1 - |forward . normal|)).
//
// The port runs the whole of it once per car from the entry of car_wall_collision_query on its first body point,
// over Rush 2's car struct (its body points +0x260, their previous positions +0x290, wheel contact points +0x230,
// position +0x224 and +0x7B0, body point forces +0xB8, body offsets +0xE8, mass +0x5B0, 1/dt +0x63C, the leaf query's
// cached nodes +0x380), then Rush 2's probe is skipped for all four points. The leaf under a body point is found with
// Rush 2's own collision_node_find_cached_8006CE28 and its cell rule (x - 0.99 truncated for negative x), so the
// wheels and the body see the same leaves. California Speed defers the hit for its cars in state 2 (+0x840) and
// applies it at once for the rest; the deferred path (the deepest hit wins, a second hit is retested after the first
// is applied) is used for every car here, Rush 2's struct having no such state.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "calspeed.h"
#include "collision_polys.h"

extern "C" void collision_node_find_cached_8006CE28(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Rush 2's collision file in RDRAM (collision_file_load_800814B0's pointers, docs/rush2049_research/collision.md).
    constexpr uint32_t node_table = 0x8010D390;   // NODE records (0x14 bytes), the root first
    constexpr uint32_t poly_table = 0x8011001C;   // POLY records (0x18: u16 flags, type in bits 0-3; u16 info, vertex
                                                  // count in bits 0-3; s16 matrix[9] x 16384, world -> the polygon's
                                                  // frame, row 1 its normal; u16 vertex list offset at +0x16)
    constexpr uint32_t vert_table = 0x80110050;   // VERT records (8: s16 x, y, z; u16 fraction bits x 14-10, y 9-5, z 4-0)
    constexpr uint32_t vlist_table = 0x80111930;  // vertex index lists
    constexpr uint32_t leaf_table = 0x8011187C;   // leaf polygon lists: u8 count, u16 entries (bits 15-13 run, 12-0 index)
    constexpr uint32_t poly_size = 0x18, vert_size = 8;
    constexpr int vlist_run_mark = 0xC0, vlist_run_mask = 0x3F;

    // The physics car (0x81C bytes).
    constexpr uint32_t car_velocity = 0x34;        // f32[3], the car's frame
    constexpr uint32_t car_angular = 0x40;         // f32[3], the car's frame
    constexpr uint32_t car_point_force = 0xB8;     // f32[4][3]: force on each body point, the car's frame
    constexpr uint32_t car_body_offset = 0xE8;     // f32[4][3]: the body points in the car's frame
    constexpr uint32_t car_position = 0x224;       // f32[3] world
    constexpr uint32_t car_wheel_point = 0x230;    // f32[4][3]: the wheels' contact points, world
    constexpr uint32_t car_body_point = 0x260;     // f32[4][3]: the body points, world
    constexpr uint32_t car_body_prev = 0x290;      // f32[4][3]: the body points as the probe left them last step
    constexpr uint32_t car_matrix = 0x2CC;         // f32[3][3]: world -> the car's frame (rows right, up, forward)
    constexpr uint32_t car_node_cache = 0x380;     // CollNode*[4]: the leaf query's cached node per body point
    constexpr uint32_t car_mass = 0x5B0;           // f32 slugs
    constexpr uint32_t car_inv_dt = 0x63C;         // f32 1 / the physics step
    constexpr uint32_t car_position_copy = 0x7B0;  // f32[3]: moved with the position by the wall response

    constexpr int point_order[4] = { 0, 1, 3, 2 };  // 0x800B1878: the body points in order round the car
    constexpr float vertex_unit = 1.0f / 32.0f;     // 0x800DCCD8: a VERT's units
    constexpr float min_depth = 0.0005f;            // 0x800DCCE0: a shallower hit is none
    constexpr float cell_bias = 0.99f;              // Rush 2's leaf query cell for a negative coordinate
    constexpr float along_gain = 0.006f;            // 0x3BC49BA6: x 10000 / mass / dt, against the velocity along a wall
    constexpr float along_gain_other = 0.015f;      // 0x3C75C28F: the same for a hit on a polygon that isn't a wall
    constexpr float into_gain = 0.1f;               // of the along gain, against the velocity into the wall
    constexpr float out_push = 2.5f;                // 0x800B0D20: x 1/dt x (1 - |forward . normal|) out of the wall
    // Test aid, RUSH2_CS_OLD_MISS=1: the port as it was before 2026-10-27 (missed wheels left as Rush 2 reads them, the
    // push out of a wall 8), to compare sims against.
    const bool old_miss = getenv("RUSH2_CS_OLD_MISS") != nullptr;

    struct Hit {
        uint32_t poly = 0;   // POLY record, 0 = none
        int point = -1;      // 0-3, in point_order: the body point the force goes to
        float depth = 0.0f;  // ft along the normal
    };
    // RUSH2_CS_WALL_LOG: the player's car (physics car 0): each wall polygon an edge of its body quad crosses, and each
    // hit applied.
    const bool wall_log = getenv("RUSH2_CS_WALL_LOG") != nullptr;
    constexpr uint32_t player_car = 0x800F5470;
    int poly_index(uint8_t* rdram, uint32_t poly) {
        return (int)((poly - (uint32_t)MEM_W(0, (int32_t)0x8011001C)) / 0x18);
    }

    gpr sx(uint32_t v) {
        return (gpr)(int64_t)(int32_t)v;
    }
    float f32(uint8_t* rdram, uint32_t at) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t at, float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)bits;
    }
    void get3(uint8_t* rdram, uint32_t at, float v[3]) {
        for (int k = 0; k < 3; k++) v[k] = f32(rdram, at + 4 * k);
    }
    void add3(uint8_t* rdram, uint32_t at, const float v[3]) {
        for (int k = 0; k < 3; k++) put_f32(rdram, at + 4 * k, f32(rdram, at + 4 * k) + v[k]);
    }
    // out = M v and out = M^T v for a row-major 3x3.
    void mul(const float m[9], const float v[3], float out[3]) {
        for (int r = 0; r < 3; r++) out[r] = m[3 * r] * v[0] + m[3 * r + 1] * v[1] + m[3 * r + 2] * v[2];
    }
    void mul_t(const float m[9], const float v[3], float out[3]) {
        for (int c = 0; c < 3; c++) out[c] = m[c] * v[0] + m[3 + c] * v[1] + m[6 + c] * v[2];
    }
    void poly_matrix(uint8_t* rdram, uint32_t poly, float m[9]) {
        for (int k = 0; k < 9; k++) m[k] = (int16_t)MEM_H(0, (int32_t)(poly + 4 + 2 * k)) / 16384.0f;
    }
    // A VERT's coordinate c (0 x, 1 y, 2 z): its integer part x 32 plus its 5 fraction bits, in 1/32 ft.
    float vert_coord(uint8_t* rdram, uint32_t vert, int c) {
        int frac = MEM_HU(6, (int32_t)vert) >> (10 - 5 * c) & 31;
        return (float)((int32_t)(int16_t)MEM_H(2 * c, (int32_t)vert) * 32 + frac) * vertex_unit;
    }
    // A polygon's vertex indices (its info's low nibble gives the count) from its run-length list: a u16 index, then
    // when at least two are left and the next byte is 0xC0 or more, that byte's low 6 bits more indices following it.
    int poly_vertices(uint8_t* rdram, uint32_t poly, uint32_t out[16]) {
        int count = MEM_HU(2, (int32_t)poly) & 0xF, n = 0;
        uint32_t p = (uint32_t)MEM_W(0, (int32_t)vlist_table) + MEM_HU(0x16, (int32_t)poly);
        while (n < count) {
            uint32_t first = (uint32_t)MEM_BU(0, (int32_t)p) << 8 | MEM_BU(1, (int32_t)p);
            p += 2;
            int run = 0;
            if (count - n >= 2 && MEM_BU(0, (int32_t)p) >= vlist_run_mark) {
                run = MEM_BU(0, (int32_t)p) & vlist_run_mask;
                p++;
            }
            for (int k = 0; k <= run && n < count && n < 16; k++) out[n++] = first + k;
        }
        return n;
    }

    // func_800B0A04: the hit is applied to the car.
    void apply_hit(uint8_t* rdram, uint32_t car, const Hit& hit) {
        float m[9], car_m[9];
        poly_matrix(rdram, hit.poly, m);
        // The hit body point's wall, as func_800B0A04 marks it (+0x862, read by the hill push).
        rush2::calspeed::physics::wall_hit(car, hit.point, MEM_HU(0, (int32_t)hit.poly) & 0xF);
        for (int k = 0; k < 9; k++) car_m[k] = f32(rdram, car + car_matrix + 4 * k);
        float normal[3] = { m[3], m[4], m[5] };
        float push[3] = { normal[0] * hit.depth, normal[1] * hit.depth, normal[2] * hit.depth };
        add3(rdram, car + car_position, push);
        add3(rdram, car + car_position_copy, push);
        for (int i = 0; i < 4; i++) {
            add3(rdram, car + car_body_point + 12 * i, push);
            add3(rdram, car + car_wheel_point + 12 * i, push);
            for (int k = 0; k < 3; k++) {
                put_f32(rdram, car + car_body_prev + 12 * i + 4 * k, f32(rdram, car + car_body_point + 12 * i + 4 * k));
            }
        }
        // The hit point's velocity: r x w + v in the car's frame, to the world, then the wall's frame.
        int pt = point_order[hit.point];
        float r[3], w[3], v[3];
        get3(rdram, car + car_body_offset + 12 * pt, r);
        get3(rdram, car + car_angular, w);
        get3(rdram, car + car_velocity, v);
        float vp[3] = { r[1] * w[2] - r[2] * w[1] + v[0], r[2] * w[0] - r[0] * w[2] + v[1], r[0] * w[1] - r[1] * w[0] + v[2] };
        float vw[3], vl[3];
        mul_t(car_m, vp, vw);
        mul(m, vw, vl);
        int type = MEM_HU(0, (int32_t)hit.poly) & 0xF;
        float inv_dt = f32(rdram, car + car_inv_dt), per_mass = 10000.0f / f32(rdram, car + car_mass);
        float along = per_mass * inv_dt * (type >= 4 && type <= 9 ? along_gain : along_gain_other);
        float facing = car_m[6] * normal[0] + car_m[7] * normal[1] + car_m[8] * normal[2];
        double out = (double)(inv_dt * (old_miss ? 8.0f : out_push)) * (1.0 - std::fabs((double)facing));
        float fl[3] = { -vl[0] * along, (float)((double)(-vl[1] * per_mass * inv_dt * into_gain) + out), -vl[2] * along };
        float fw[3], fc[3];
        mul_t(m, fl, fw);
        mul(car_m, fw, fc);
        add3(rdram, car + car_point_force + 12 * pt, fc);
        rush2_rumble_wall_contact(rdram, car, normal);
        if (wall_log && car == player_car) {
            printf("[cswall] hit poly %d type %d depth %.3f point %d normal (%.2f %.2f %.2f) force (%.0f %.0f %.0f) "
                   "mass %.1f invdt %.1f vl (%.1f %.1f %.1f) v (%.1f %.1f %.1f) w (%.2f %.2f %.2f)\n",
                   poly_index(rdram, hit.poly), type, hit.depth, pt, normal[0], normal[1], normal[2], fc[0], fc[1], fc[2],
                   f32(rdram, car + car_mass), inv_dt, vl[0], vl[1], vl[2], v[0], v[1], v[2], w[0], w[1], w[2]);
        }
    }

    // func_800B0E80: the body quad's edges against one wall polygon; a hit deeper than the one held is applied in its
    // place (the shallower retested against the moved car), the first is held.
    void test_wall(uint8_t* rdram, uint32_t car, uint32_t poly, Hit& held) {
        if (held.poly == poly) return;
        uint32_t idx[16];
        int count = poly_vertices(rdram, poly, idx);
        if (count < 3) return;
        uint32_t verts = (uint32_t)MEM_W(0, (int32_t)vert_table);
        float origin[3], m[9];
        for (int c = 0; c < 3; c++) origin[c] = vert_coord(rdram, verts + vert_size * idx[0], c);
        poly_matrix(rdram, poly, m);
        float local[4][3];
        for (int k = 0; k < 4; k++) {
            float p[3];
            get3(rdram, car + car_body_point + 12 * point_order[k], p);
            for (int c = 0; c < 3; c++) p[c] -= origin[c];
            mul(m, p, local[k]);
        }
        float vx[16], vz[16];
        for (int k = 1; k < count; k++) {
            vx[k] = vert_coord(rdram, verts + vert_size * idx[k], 0);
            vz[k] = vert_coord(rdram, verts + vert_size * idx[k], 2);
        }
        float best = 0.0f;
        int point = -1;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            float y0 = local[i][1], y1 = local[j][1];
            if (!(y0 * y1 < 0.0f)) continue;   // the edge crosses the plane
            if (wall_log && car == player_car) {
                printf("[cswall] poly %d edge %d-%d y %.2f %.2f at (%.1f %.1f)\n", poly_index(rdram, poly), i, j, y0, y1,
                       local[i][0] + (local[j][0] - local[i][0]) * std::fabs(y0 / (y1 - y0)),
                       local[i][2] + (local[j][2] - local[i][2]) * std::fabs(y0 / (y1 - y0)));
            }
            float dy = y1 - y0, dx = local[j][0] - local[i][0], dz = local[j][2] - local[i][2];
            if (dy == 0.0f) continue;
            float t = std::fabs(y0 / dy);
            float xc = local[i][0] + dx * t, zc = local[i][2] + dz * t;   // the crossing
            float f;   // the fraction of the edge behind the plane, from the crossing
            if (dy > 0.0f) {
                dx = -dx;
                dz = -dz;
                f = t;
            }
            else {
                f = 1.0f - t;
            }
            // Clipped to the footprint: the crossing must be inside each edge of the polygon, and the part behind the
            // plane is cut where it leaves. Edge 0-1 lies along the frame's x axis.
            if (-vx[1] * zc > 0.0f) continue;
            if (dz != 0.0f) {
                float s = -zc / dz;
                if (s > 0.0f && s < f) f = s;
            }
            bool outside = false;
            for (int k = 2; k < count && !outside; k++) {
                float ax = vx[k - 1], az = vz[k - 1], bx = vx[k], bz = vz[k];
                float cross = (bz - az) * (xc - ax) - (bx - ax) * (zc - az);
                if (cross > 0.0f) {
                    outside = true;
                    break;
                }
                float den = (bx - ax) * dz - (bz - az) * dx;
                if (den != 0.0f) {
                    float s = cross / den;
                    if (s > 0.0f && s < f) f = s;
                }
            }
            if (outside) continue;
            float ax = vx[count - 1], az = vz[count - 1];
            float cross = az * xc - ax * zc;
            if (cross < 0.0f) continue;
            float den = ax * dz - az * dx;
            if (den != 0.0f) {
                float s = cross / den;
                if (s > 0.0f && s < f) f = s;
            }
            float depth = std::fabs(f * dy);
            if (wall_log && car == player_car) printf("[cswall]   inside, fraction %.3f depth %.3f\n", f, depth);
            if (depth > best) {
                best = depth;
                point = -y0 / y1 < 1.0f ? i : j;   // this end when it is the nearer to the plane, else the other
            }
        }
        if (point < 0 || (double)best < (double)min_depth) return;
        if (held.poly != 0) {
            if (held.depth < best) {
                apply_hit(rdram, car, held);
                held = Hit{};
                test_wall(rdram, car, poly, held);
            }
            else {
                uint32_t other = held.poly;
                held = Hit{ poly, point, best };
                apply_hit(rdram, car, held);
                held = Hit{};
                test_wall(rdram, car, other, held);
            }
            return;
        }
        held = Hit{ poly, point, best };
    }

    // The leaf polygon list under world (x, z) for body point i, found as Rush 2's probe finds it, or 0.
    uint32_t leaf_list(uint8_t* rdram, recomp_context* ctx, uint32_t car, int i, float x, float z) {
        int32_t cx = (int32_t)(x < 0.0f ? x - cell_bias : x), cz = (int32_t)(z < 0.0f ? z - cell_bias : z);
        uint32_t scratch = (uint32_t)ctx->r29 - 0x400;   // below the probe's frame, which isn't set up yet
        uint32_t cache = (uint32_t)MEM_W(0, (int32_t)(car + car_node_cache + 4 * i));
        if (cache == 0) cache = (uint32_t)MEM_W(0, (int32_t)node_table);
        if (cache == 0) return 0;
        recomp_context call = *ctx;
        call.r29 = sx(scratch);
        call.r4 = sx(cache);
        call.r5 = (gpr)(int64_t)(int16_t)cx;
        call.r6 = (gpr)(int64_t)(int16_t)cz;
        call.r7 = sx(scratch + 0x10);
        collision_node_find_cached_8006CE28(rdram, &call);
        uint32_t node = (uint32_t)call.r2;
        if (node == 0) return 0;
        int q = (int16_t)MEM_H(0, (int32_t)(scratch + 0x10)) & 3;
        uint32_t offset = rush2::collision::leaf_offset(rdram, node, q);   // with the extended scenery's far leaves
        if (offset == 0) return 0;
        return (uint32_t)MEM_W(0, (int32_t)leaf_table) + offset;
    }
}

// car_wall_collision_query_8006FC94's entry ($a0 = the car, 0x10($sp) = the body point 0-3): on a California Speed
// track, its wall collision for the whole car on point 0, and Rush 2's probe skipped for every point (returns 1).
extern "C" int rush2_calspeed_wall_collision(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    if (!rush2::calspeed::physics::active(rdram, car)) return 0;
    int index = (int32_t)MEM_W(0x10, ctx->r29);
    if (index != 0) return 1;
    // func_800B1624: the leaves under the body points, each once.
    uint32_t lists[4];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t list = leaf_list(rdram, ctx, car, i, f32(rdram, car + car_body_point + 12 * i),
                                  f32(rdram, car + car_body_point + 12 * i + 8));
        if (list == 0 || std::find(lists, lists + n, list) != lists + n) continue;
        lists[n++] = list;
    }
    Hit held;
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table);
    if (wall_log && car == player_car) {
        static int calls = 0;
        if (++calls % 300 == 1) {
            printf("[cswall] car at (%.0f %.0f %.0f): %d leaves\n", f32(rdram, car + car_position),
                   f32(rdram, car + car_position + 4), f32(rdram, car + car_position + 8), n);
        }
    }
    // A custom track past 8,191 polygons has plain u16 entries (rush2::collision::wide_leaves).
    bool wide = rush2::collision::wide_leaves(rdram);
    for (int l = 0; l < n; l++) {
        int count = MEM_BU(0, (int32_t)lists[l]), done = 0;
        uint32_t p = lists[l] + 1;
        while (done < count) {
            // The entries follow the count byte unaligned: two byte reads.
            uint32_t entry = (uint32_t)MEM_BU(0, (int32_t)p) << 8 | MEM_BU(1, (int32_t)p);
            p += 2;
            uint32_t run = wide ? 0 : entry >> 13, first = wide ? entry : entry & 0x1FFF;
            for (uint32_t k = 0; k <= run && done < count; k++, done++) {
                uint32_t poly = polys + poly_size * (first + k);
                if ((MEM_HU(0, (int32_t)poly) & 0xF) >= 4) test_wall(rdram, car, poly, held);
            }
        }
    }
    if (held.poly != 0) apply_hit(rdram, car, held);
    // Rush 2's probe leaves each body point's position as its previous one for the next step.
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++) {
            put_f32(rdram, car + car_body_prev + 12 * i + 4 * k, f32(rdram, car + car_body_point + 12 * i + 4 * k));
        }
    }
    return 1;
}

// California Speed's rule for a wheel that finds no road (docs/calspeed_research.md#a-missed-wheel-keeps-its-compression).
//
// Rush 2's car_ground_contact_800703EC probes each wheel on its own. A wheel whose probe finds no polygon
// (collision_poly_extract_8006F704's miss path, 0x8006FBD0) gets surface type 1 and a height of its world y - 200
// (0x8006FBFC), so it reads as off the road (compression -3, type 8) only above world y 197; below that, which is
// nearly all of every California Speed course, it reads as fully compressed (+3) on dirt. Against a wall the wheels
// that poke past its plane read so, Rush 2's springs and the road hold (factor 0.82 - 3 per wheel) push the car along
// its up axis, and it drives up the wall (docs/calspeed_research.md#wall-climbing-a-missed-wheel-reads-as-fully-compressed-fixed-2026-10-27).
// California Speed's func_800A58A4 counts the wheels whose probe hit first, and only when fewer than three did does a
// missed wheel get the far surface (-1000 ft, clamped to -3); with three or more hits a missed wheel keeps last
// step's compression, surface type and polygon, as if the road were still under it. Its probe misses now and then on
// any course (a wheel over a seam, or a polygon the planar window turns down on a steep bank), and on the loop of its
// San Francisco course a single miss would otherwise flip the road hold to world-down and peel the car off the wall.
// Here the miss path notes each missed wheel (rush2_calspeed_probe_miss), the car's wheel fields are saved at the
// entry of car_ground_contact, and after its wheel loop a missed wheel is put back with three hits (its compression
// speed 0, the compression being unchanged, as California Speed's would be) and otherwise put off the road as Rush 2
// does a wheel with no road under it: compression -3, travel 1000, offset 0, type and material 8, its travel speed
// from the change clamped to 40.
namespace {
    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr int max_cars = 8;
    constexpr int32_t miss_matrix = (int32_t)0x800CA27C;  // fog_zone_lists+0x84: the matrix a missed wheel gets
    // The MEM_ macros take a sign-extended address (they subtract 0xFFFFFFFF80000000), so the car is an int32_t.

    struct WheelState {
        uint32_t words[4][4];     // +0x5BC compression, +0x5CC last height, +0x5DC travel, +0x5EC travel speed
        uint32_t type[4];         // +0x5FC surface type
        uint16_t halves[4][3];    // +0x60C, +0x614, +0x61C
        uint32_t matrix[4][9];    // +0x2F0 + 0x24 i, world -> polygon
        uint32_t point[4][3];     // +0x230 + 12 i, the contact point
    };
    WheelState saved[max_cars];
    bool probe_missed[max_cars][4];   // this step's probe found no polygon for the wheel

    int car_index(uint32_t car) {
        if (car < physics_cars || (car - physics_cars) % physics_car_size != 0) return -1;
        uint32_t i = (car - physics_cars) / physics_car_size;
        return i < uint32_t(max_cars) ? int(i) : -1;
    }

    // California Speed's off-the-road wheel: its surface 1000 ft away (func_800A58A4), so travel +0x5DC = 1000 and
    // the compression clamped to -3.
    constexpr float off_travel = 1000.0f;
}

extern "C" void rush2_calspeed_ground_contact_enter(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::calspeed::physics::active(rdram, uint32_t(ctx->r4))) return;
    rush2::calspeed::physics::ground_contact_begin(rdram, uint32_t(ctx->r4));
    int n = car_index(uint32_t(ctx->r4));
    if (n < 0) return;
    int32_t car = int32_t(ctx->r4);
    WheelState& s = saved[n];
    for (bool& m : probe_missed[n]) m = false;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) s.words[i][k] = MEM_W(0x5BC + 0x10 * k + 4 * i, car);
        s.type[i] = MEM_W(0x5FC + 4 * i, car);
        for (int k = 0; k < 3; k++) s.halves[i][k] = MEM_HU(0x60C + 8 * k + 2 * i, car);
        for (int k = 0; k < 9; k++) s.matrix[i][k] = MEM_W(0x2F0 + 0x24 * i + 4 * k, car);
        for (int k = 0; k < 3; k++) s.point[i][k] = MEM_W(0x230 + 12 * i + 4 * k, car);
    }
}

namespace {
    void keep_missed_wheels(uint8_t* rdram, int n, int32_t car);
}

// collision_poly_extract_8006F704 at 0x8006FBD0, no polygon held the wheel's point (and smooth collision found none
// within its gap): the wheel (0x14C($sp)) of the car (0x138($sp)) missed.
extern "C" void rush2_calspeed_probe_miss(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)MEM_W(0x138, ctx->r29);
    int wheel = (int)MEM_W(0x14C, ctx->r29);
    int n = car_index(car);
    if (n < 0 || wheel < 0 || wheel > 3 || !rush2::calspeed::physics::active(rdram, car)) return;
    probe_missed[n][wheel] = true;
}

extern "C" void rush2_calspeed_ground_contact_leave(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::calspeed::physics::active(rdram, uint32_t(ctx->r17))) return;
    int n = car_index(uint32_t(ctx->r17));
    if (n < 0) return;
    if (!old_miss) keep_missed_wheels(rdram, n, int32_t(ctx->r17));
    rush2::calspeed::physics::ground_contact_end(rdram, uint32_t(ctx->r17));
}

namespace {
// The missed-wheel rule itself, after car_ground_contact's wheel loop.
void keep_missed_wheels(uint8_t* rdram, int n, int32_t car) {
    int hits = 0;
    bool miss[4];
    for (int i = 0; i < 4; i++) {
        miss[i] = probe_missed[n][i];
        hits += !miss[i];
    }
    static const bool diag = getenv("RUSH2_CS_GROUND_DIAG") != nullptr;
    if (diag) {
        for (int i = 0; i < 4; i++) {
            if (MEM_W(0x5FC + 4 * i, car) != 8 && f32(rdram, car + 0x5BC + 4 * i) >= 0.0f) continue;
            printf("[csdiag] car %d wheel %d comp %.2f type %u half %u matrix %.2f %.2f %.2f / miss %.2f %.2f %.2f "
                   "missed %d\n", n, i, f32(rdram, car + 0x5BC + 4 * i), unsigned(MEM_W(0x5FC + 4 * i, car)),
                   unsigned(MEM_HU(0x60C + 2 * i, car)), f32(rdram, car + 0x2F0 + 0x24 * i + 12),
                   f32(rdram, car + 0x2F0 + 0x24 * i + 16), f32(rdram, car + 0x2F0 + 0x24 * i + 20),
                   f32(rdram, miss_matrix + 12), f32(rdram, miss_matrix + 16), f32(rdram, miss_matrix + 20), int(miss[i]));
        }
    }
    if (hits == 4) return;
    static const bool log = getenv("RUSH2_CS_WALL_LOG") != nullptr;
    const WheelState& s = saved[n];
    for (int i = 0; i < 4; i++) {
        if (!miss[i]) continue;
        // Fewer than three hits, or already off the road last step: off the road.
        if (hits < 3 || (s.type[i] == 8 && s.words[i][0] == 0xC0400000u)) {
            union { uint32_t u; float f; } old = { s.words[i][2] };
            float speed = std::clamp((off_travel - old.f) * f32(rdram, car + 0x63C), -40.0f, 40.0f);
            put_f32(rdram, car + 0x5BC + 4 * i, -3.0f);
            put_f32(rdram, car + 0x5CC + 4 * i, 0.0f);
            put_f32(rdram, car + 0x5DC + 4 * i, off_travel);
            put_f32(rdram, car + 0x5EC + 4 * i, speed);
            MEM_W(0x5FC + 4 * i, car) = 8;
            MEM_H(0x60C + 2 * i, car) = 8;
            continue;
        }
        for (int k = 0; k < 4; k++) MEM_W(0x5BC + 0x10 * k + 4 * i, car) = s.words[i][k];
        MEM_W(0x5EC + 4 * i, car) = 0;
        MEM_W(0x5FC + 4 * i, car) = s.type[i];
        for (int k = 0; k < 3; k++) MEM_H(0x60C + 8 * k + 2 * i, car) = int16_t(s.halves[i][k]);
        for (int k = 0; k < 9; k++) MEM_W(0x2F0 + 0x24 * i + 4 * k, car) = s.matrix[i][k];
        for (int k = 0; k < 3; k++) MEM_W(0x230 + 12 * i + 4 * k, car) = s.point[i][k];
        if (log) {
            union { uint32_t u; float f; } c = { s.words[i][0] };
            printf("[csground] car %d wheel %d missed with %d hits: kept compression %.2f at %.1f %.1f %.1f\n", n, i,
                   hits, c.f, f32(rdram, car + 0x224), f32(rdram, car + 0x228), f32(rdram, car + 0x22C));
        }
    }
}
}
