// Smooth-shaded collision: a road surface whose tilt changes smoothly across polygon edges, for California Speed's
// tracks and custom mod tracks (docs/custom_tracks.md#smooth-collision).
//
// Rush 2's road is flat polygons. A wheel takes the polygon under it: its height above that polygon's plane along
// the plane's normal, and the polygon's frame (rows across, normal, along; car +0x2F0 + 0x24 x wheel) that the
// suspension and tire forces work in. Two flat polygons meeting at a crease share their edge, so the height is
// continuous across it, but the normal jumps: the wheel's velocity along the new normal jumps with it, the damper
// and the tire see a spike, and the car is kicked. The stock tracks hide the creases of their main roads under the
// spline road (poly flag 0x1000, collision_seg_surface_8006BDA8), whose frame turns smoothly along a segment. A
// converted track has no splines (California Speed's are only on parts of its roads, a mod's only where its author
// built them), so every edge of its road is a crease.
//
// This gives those tracks' floor polygons a smooth normal instead: every vertex gets the mean normal of the floor
// polygons that meet at its position (the same position, to 1/32 ft, since the files repeat vertices), leaving out
// the neighbors that fold away by more than the fold angle so a curb, a ramp lip or a jump edge stays sharp; under
// a wheel the normal is the barycentric blend of the polygon's corner normals, and the wheel's frame is the flat
// frame turned onto it. Two polygons sharing an edge blend the same two corner normals along it, so the normal is
// continuous across the edge. The height stays the flat polygon's (the distance to the flat face, taken along the
// blended normal), so the surface a car rests on is exactly the mesh its author built and the probe's flat-face
// polygon pick still applies; only the tilt the physics sees is smoothed.
//
// Hooked in collision_poly_extract_8006F704 at 0x8006FA70, after the surface dispatch (flat plane, or the spline or
// arc surface for flags 0x1000 / 0x2000, which are left alone): 0x94($sp) = the POLY found, 0x13C($sp) = the wheel's
// point, $s1 = the result vector whose [1] is the height, *0x148($sp) = the wheel's frame (f32[3][3], world -> the
// polygon's frame). The corner normals are built once per loaded collision file (keyed on the POLY and VERT table
// pointers), on the first wheel probe of a race, from the file in RDRAM.
//
// RUSH2_SMOOTH_FOLD=<degrees> overrides the fold angle (default 20); RUSH2_SMOOTH_COLLISION=0 turns the surface off;
// RUSH2_SMOOTH_LOG=1 prints the table's statistics when it is built.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "collision_polys.h"
#include "calspeed.h"
#include "track2049.h"

namespace {
    // Rush 2's collision file pointers (collision_file_load_800814B0, docs/rush2049_research/collision.md).
    constexpr uint32_t poly_table_ptr = 0x8011001C;   // POLY records (0x18 bytes)
    constexpr uint32_t vert_table_ptr = 0x80110050;   // VERT records (8 bytes), right after the POLY records
    constexpr uint32_t vlist_table_ptr = 0x80111930;  // vertex index lists
    constexpr uint32_t poly_size = 0x18, vert_size = 8;
    constexpr int vlist_run_mark = 0xC0, vlist_run_mask = 0x3F;
    constexpr uint32_t flag_spline = 0x1000, flag_arc = 0x2000;
    constexpr int max_corners = 15;                   // a POLY's vertex count is its info's low nibble

    // The probe's frame at the hook.
    // Stack slot offsets are int: MEM_W adds the offset to the (int32) base before widening it to the 64-bit address, so an
    // unsigned offset wraps the sum to 32 bits and the access lands 4 GB away.
    constexpr int32_t sp_poly = 0x94, sp_point = 0x13C, sp_frame = 0x148;

    struct Corner {
        float p[3];   // world position
        float n[3];   // smooth normal
    };
    struct Smooth {
        int count = 0;                  // corners, 0 = not a floor polygon
        int first = 0;                  // into corners
        float face[3] = {};             // the flat normal (the frame's row 1)
    };
    struct Table {
        uint32_t polys = 0, verts = 0;  // the file it was built for
        float fold_cos = 0.0f;
        std::vector<Smooth> smooth;
        std::vector<Corner> corners;
    };
    Table table;

    const bool enabled = [] {
        const char* v = getenv("RUSH2_SMOOTH_COLLISION");
        return v == nullptr || strcmp(v, "0") != 0;
    }();
    const bool log_on = getenv("RUSH2_SMOOTH_LOG") != nullptr;
    // RUSH2_SMOOTH_LOG=2: every probe of the player's car whose height is more than 1 ft off the usual rest (a kick),
    // with the polygon found and what the dispatch left (before the smooth surface).
    const bool log_probes = log_on && atoi(getenv("RUSH2_SMOOTH_LOG")) >= 2;
    constexpr uint32_t player_car = 0x800F5470;
    constexpr int32_t sp_car = 0x138;
    // The fold angle: the raced custom track's smooth_fold, else 20 degrees; RUSH2_SMOOTH_FOLD overrides both.
    float fold_degrees(uint8_t* rdram) {
        if (const char* v = getenv("RUSH2_SMOOTH_FOLD")) return (float)atof(v);
        float fold = 20.0f;
        if (!rush2::calspeed::racing(rdram)) rush2::track2049::mod_smooth_collision(rdram, &fold);
        return fold;
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
    float vert_coord(uint8_t* rdram, uint32_t vert, int c) {
        int frac = MEM_HU(6, (int32_t)vert) >> (10 - 5 * c) & 31;
        return (float)((int32_t)(int16_t)MEM_H(2 * c, (int32_t)vert) * 32 + frac) / 32.0f;
    }
    void poly_matrix(uint8_t* rdram, uint32_t poly, float m[9]) {
        for (int k = 0; k < 9; k++) m[k] = (int16_t)MEM_H(0, (int32_t)(poly + 4 + 2 * k)) / 16384.0f;
    }
    int poly_vertices(uint8_t* rdram, uint32_t poly, uint32_t out[16]) {
        int count = MEM_HU(2, (int32_t)poly) & 0xF, n = 0;
        uint32_t p = (uint32_t)MEM_W(0, (int32_t)vlist_table_ptr) + MEM_HU(0x16, (int32_t)poly);
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
    void normalize(float v[3]) {
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) {
            v[0] /= len;
            v[1] /= len;
            v[2] /= len;
        }
    }
    uint64_t position_key(const float p[3]) {
        // 1/32 ft cells: the files' vertices are 1/32 ft fixed point, so repeated vertices land in the same cell.
        uint64_t k = 0;
        for (int c = 0; c < 3; c++) k = k * 0x100000 + ((uint64_t)(int64_t)std::lround(p[c] * 32.0f) & 0xFFFFF);
        return k;
    }

    // A polygon's vertices in the world: vertex 0 is the origin, the others are in its frame (rows = world -> frame).
    int world_vertices(uint8_t* rdram, uint32_t poly, float out[16][3], float m[9]) {
        uint32_t idx[16];
        int count = poly_vertices(rdram, poly, idx);
        uint32_t verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
        poly_matrix(rdram, poly, m);
        float o[3];
        for (int c = 0; c < 3; c++) o[c] = vert_coord(rdram, verts + vert_size * idx[0], c);
        for (int k = 0; k < count; k++) {
            float v[3];
            for (int c = 0; c < 3; c++) v[c] = vert_coord(rdram, verts + vert_size * idx[k], c);
            for (int c = 0; c < 3; c++) out[k][c] = k == 0 ? o[c] : o[c] + m[c] * v[0] + m[3 + c] * v[1] + m[6 + c] * v[2];
        }
        return count;
    }

    // Builds the corner normals for the loaded file.
    void build(uint8_t* rdram, float fold_cos) {
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
        table = Table{};
        table.polys = polys;
        table.verts = verts;
        table.fold_cos = fold_cos;
        if (polys == 0 || verts <= polys) return;
        int n = (int)((verts - polys) / poly_size);
        if (n <= 0 || n > 0x4000) return;
        table.smooth.resize(n);
        struct Use {
            int poly;
            float n[3];
        };
        std::unordered_map<uint64_t, std::vector<Use>> at;
        at.reserve((size_t)n * 3);
        std::vector<std::vector<Corner>> raw(n);
        for (int i = 0; i < n; i++) {
            uint32_t poly = polys + poly_size * (uint32_t)i;
            uint32_t flags = MEM_HU(0, (int32_t)poly);
            if ((flags & 0xF) >= 4 || (flags & (flag_spline | flag_arc))) continue;   // walls; spline and arc roads
            float w[16][3], m[9];
            int count = world_vertices(rdram, poly, w, m);
            if (count < 3 || count > max_corners) continue;
            Smooth& s = table.smooth[i];
            s.count = count;
            for (int c = 0; c < 3; c++) s.face[c] = m[3 + c];
            normalize(s.face);
            raw[i].resize(count);
            for (int k = 0; k < count; k++) {
                memcpy(raw[i][k].p, w[k], sizeof w[k]);
                at[position_key(w[k])].push_back(Use{ i, { s.face[0], s.face[1], s.face[2] } });
            }
        }
        int smoothed = 0, sharp = 0;
        for (int i = 0; i < n; i++) {
            Smooth& s = table.smooth[i];
            if (s.count == 0) continue;
            s.first = (int)table.corners.size();
            for (int k = 0; k < s.count; k++) {
                Corner c = raw[i][k];
                float sum[3] = {};
                int used = 0;
                for (const Use& u : at[position_key(c.p)]) {
                    float d = u.n[0] * s.face[0] + u.n[1] * s.face[1] + u.n[2] * s.face[2];
                    if (d < fold_cos) continue;   // folds away more than the fold angle: a crease to keep
                    for (int a = 0; a < 3; a++) sum[a] += u.n[a];
                    used++;
                }
                normalize(sum);
                if (used <= 1 || sum[0] * s.face[0] + sum[1] * s.face[1] + sum[2] * s.face[2] < 0.5f) {
                    memcpy(c.n, s.face, sizeof c.n);
                    sharp++;
                }
                else {
                    memcpy(c.n, sum, sizeof c.n);
                    smoothed++;
                }
                table.corners.push_back(c);
            }
        }
        if (log_on) {
            printf("[smooth] collision file %08X: %d polygons, %zu floor polygons, %d corners smoothed, %d sharp, fold cos %.3f\n",
                   polys, n, table.corners.size(), smoothed, sharp, fold_cos);
            fflush(stdout);
        }
    }

    bool wanted(uint8_t* rdram) {
        if (!enabled) return false;
        // California Speed's physics (CS Physics option) take its own probe: the highest floor holding the point, as
        // Rush 2's own (src/calspeed/calspeed_physics.cpp). The race's cars took their roles at their setup.
        if (rush2::calspeed::racing(rdram)) return !rush2::calspeed::physics::active(rdram, 0x800F5470);
        // A custom track's: its track.toml smooth_collision (the author's opt-out).
        return rush2::track2049::mod_smooth_collision(rdram);
    }

    // Cracks and ledges. California Speed's floors (and converted custom tracks) have hairline gaps between neighboring
    // polygons, and raised sidewalks and subway platforms a couple of feet above the road's edge whose tops are plain
    // floor polygons with no wall face on their edge, overlapping the road's edge polygons. Rush 2's probe takes only
    // the polygons whose footprint holds the wheel's point exactly, and of those the highest within 5 ft above the
    // wheel: at a crack a wheel finds no floor for a frame, and at a ledge the wheel point that strays under the
    // sidewalk's top is lifted onto it in one step; either way the car is kicked. Here the surface nearest the wheel
    // wins instead, a floor polygon whose footprint is within gap_tol of the point counting too: a ledge more than
    // ledge_step above a wheel that is on a surface is left alone, as a wall face would have stopped it.
    constexpr float gap_tol = 1.0f;       // ft outside a footprint, in the polygon's plane
    constexpr float ledge_step = 0.75f;   // a found surface this far above the wheel's point gives way to one it is on
    constexpr float ledge_rise = 0.1f;    // ... and with none to give way to, the wheel climbs it this much per probe
    // The leaf's polygon indices (u16), their count noted by the leaf hook: the probe's list, moved off its stack (sp+0xA4)
    // to spare RDRAM by rush2_collision_leaf_list (query 1) so a leaf can hold 255.
    int leaf_count = 0;                   // the current probe's leaf polygon count (0 when it found no leaf)

    // A ledge with nothing under it to keep (a sidewalk's or platform's top the wheel's point has fully entered): the
    // height is clamped so the wheel climbs it at ledge_rise per probe (a 1.9 ft platform over about a car length at
    // speed) instead of being lifted onto it in one step, which launches the car.
    // A car coming down fast (a landing: car-frame vertical velocity +0x38 under -10 ft/s) is not on a ledge; it keeps
    // the probe's full height so the floor holds it.
    constexpr float falling_speed = -10.0f;
    bool falling(uint8_t* rdram, uint32_t sp) {
        return f32(rdram, (uint32_t)MEM_W(sp_car, (int32_t)sp) + 0x38) < falling_speed;
    }
    void ledge_clamp(uint8_t* rdram, recomp_context* ctx) {
        float h = f32(rdram, (uint32_t)ctx->r17 + 4);
        if (h < -ledge_step && !falling(rdram, (uint32_t)ctx->r29)) put_f32(rdram, (uint32_t)ctx->r17 + 4, -ledge_rise);
    }

    // Distance from (lx, lz) to a polygon's footprint in its local plane, 0 inside.
    float footprint_distance(const float (*lv)[3], int count, float lx, float lz) {
        bool inside = true;
        int sign = 0;
        float best = 1e30f;
        for (int k = 0; k < count; k++) {
            const float* a = lv[k];
            const float* b = lv[(k + 1) % count];
            float ex = b[0] - a[0], ez = b[2] - a[2], len2 = ex * ex + ez * ez;
            float cross = ex * (lz - a[2]) - ez * (lx - a[0]);
            if (std::fabs(cross) > 1e-9f) {
                int s = cross > 0 ? 1 : -1;
                if (sign == 0) sign = s;
                else if (s != sign) inside = false;
            }
            float t = len2 > 0 ? std::clamp(((lx - a[0]) * ex + (lz - a[2]) * ez) / len2, 0.0f, 1.0f) : 0.0f;
            float dx = lx - a[0] - t * ex, dz = lz - a[2] - t * ez;
            best = std::min(best, std::sqrt(dx * dx + dz * dz));
        }
        return inside ? 0.0f : best;
    }

    // The tolerant search over the probe's leaf: `have` says a polygon was found (its height in $s1[1]); it is kept
    // unless it is a ledge (more than ledge_step above the point). Replaces the probe's result (POLY slot, local point,
    // frame, the car's wheel polygon index) with the floor polygon within gap_tol of the point whose surface is nearest
    // the point. Returns the polygon taken, 0 for none.
    uint32_t fill_gap(uint8_t* rdram, recomp_context* ctx, bool have) {
        uint32_t sp = (uint32_t)ctx->r29;
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr);
        if (polys == 0 || leaf_count <= 0) return 0;
        uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp);
        float p[3] = { f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8) };
        float held = have ? f32(rdram, (uint32_t)ctx->r17 + 4) : 250.0f;
        if (have && (held >= -ledge_step || falling(rdram, sp))) return 0;   // on or above the surface, or landing
        float best = have ? -held - 0.05f : 250.0f;       // the nearest |height| so far
        uint32_t taken = 0;
        float tl[3] = {}, tm[9] = {};
        int tindex = 0;
        for (int i = 0; i < leaf_count; i++) {
            int index = MEM_HU(2 * i, (int32_t)rush2_collision_leaf_list_at(1));
            uint32_t poly = polys + poly_size * (uint32_t)index;
            uint32_t flags = MEM_HU(0, (int32_t)poly);
            if ((flags & 0xF) >= 4) continue;             // walls and the like: floors only
            float w[16][3], m[9];
            int count = world_vertices(rdram, poly, w, m);
            if (count < 3 || m[4] < 0.5f) continue;       // a floor faces up
            float d[3] = { p[0] - w[0][0], p[1] - w[0][1], p[2] - w[0][2] };
            float l[3];
            for (int r = 0; r < 3; r++) l[r] = m[3 * r] * d[0] + m[3 * r + 1] * d[1] + m[3 * r + 2] * d[2];
            if (l[1] <= -5.0f || std::fabs(l[1]) >= best) continue;   // the probe's window, nearer than what is held
            float lv[16][3];
            for (int k = 0; k < count; k++) {
                float e[3] = { w[k][0] - w[0][0], w[k][1] - w[0][1], w[k][2] - w[0][2] };
                for (int r = 0; r < 3; r++) lv[k][r] = m[3 * r] * e[0] + m[3 * r + 1] * e[1] + m[3 * r + 2] * e[2];
            }
            float outside = footprint_distance(lv, count, l[0], l[2]);
            if (outside > gap_tol) continue;
            if (outside > 0.0f && m[4] < 0.94f) continue;  // a slope's plane extended past its edge is not the road
            best = std::fabs(l[1]);
            taken = poly;
            tindex = index;
            memcpy(tl, l, sizeof tl);
            memcpy(tm, m, sizeof tm);
        }
        if (taken == 0) return 0;
        MEM_W(sp_poly, (int32_t)sp) = (int32_t)taken;
        for (int r = 0; r < 3; r++) put_f32(rdram, (uint32_t)ctx->r17 + 4 * (uint32_t)r, tl[r]);
        uint32_t frame = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
        for (int k = 0; k < 9; k++) put_f32(rdram, frame + 4 * (uint32_t)k, tm[k]);
        uint32_t car = (uint32_t)MEM_W(sp_car, (int32_t)sp);
        int wheel = (int)MEM_W(0x14C, (int32_t)sp);
        MEM_H(0x594 + 2 * wheel, (int32_t)car) = (int16_t)tindex;
        ctx->r2 = MEM_HU(0, (int32_t)taken);              // $v0 = the flags, as the code at 0x8006FA70 expects
        if (log_probes && car == player_car) {
            printf("[smooth] crack wheel %d: %s poly %d h %.2f at (%.1f %.1f %.1f)\n", wheel, have ? "ledge" : "crack",
                   tindex, tl[1], p[0], p[1], p[2]);
            fflush(stdout);
        }
        return taken;
    }
}

// collision_poly_extract_8006F704 at 0x8006F748 (entry, count 0) and 0x8006F860 (the leaf decoded, $t1 = its polygon
// count): remembers the probe's leaf size for the crack search.
extern "C" void rush2_smooth_collision_leaf(uint8_t* rdram, recomp_context* ctx, int count) {
    (void)rdram;
    (void)ctx;
    leaf_count = count;
}

// collision_poly_extract_8006F704 at 0x8006FBD0: no polygon held the wheel's point. On a California Speed or custom
// track a floor polygon within gap_tol of it is taken instead; true means the result is in place and the hook text
// continues at 0x8006FA70 (type, material, frame kept flat: a spline polygon's edge is on its plane).
extern "C" bool rush2_smooth_collision_gap(uint8_t* rdram, recomp_context* ctx) {
    if (!wanted(rdram)) return false;
    return fill_gap(rdram, ctx, false) != 0;
}

// collision_poly_extract_8006F704 at 0x8006FA70: the found polygon's surface is in place (flat, spline or arc). On a
// California Speed or custom track, a flat floor polygon's frame and height are replaced by the smooth surface's.
extern "C" void rush2_smooth_collision_surface(uint8_t* rdram, recomp_context* ctx) {
    uint32_t sp = (uint32_t)ctx->r29;
    uint32_t poly = (uint32_t)MEM_W(sp_poly, (int32_t)sp);
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
    static int calls = 0;
    if (log_on && (calls++ % 4000) == 0) {
        printf("[smooth] probe call %d: wanted %d poly %08X polys %08X verts %08X sp %08X s1 %08X car %08X point %08X type %08X frame %08X wheel %d h %.2f\n",
               calls, (int)wanted(rdram), poly, polys, verts, sp, (uint32_t)ctx->r17, (uint32_t)MEM_W(0x138, (int32_t)sp),
               (uint32_t)MEM_W(0x13C, (int32_t)sp), (uint32_t)MEM_W(0x144, (int32_t)sp), (uint32_t)MEM_W(0x148, (int32_t)sp),
               (int)MEM_W(0x14C, (int32_t)sp), f32(rdram, (uint32_t)ctx->r17 + 4));
        fflush(stdout);
    }
    if (!wanted(rdram)) return;
    if (polys == 0 || poly < polys) return;
    if (uint32_t nearer = fill_gap(rdram, ctx, true)) poly = nearer;   // a ledge gives way to the road under it
    if (log_probes && (uint32_t)MEM_W(sp_car, (int32_t)sp) == player_car) {
        float h0 = f32(rdram, (uint32_t)ctx->r17 + 4);
        if (h0 < -1.0f || h0 > 2.0f) {
            uint32_t pointp0 = (uint32_t)MEM_W(sp_point, (int32_t)sp), frame0 = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
            printf("[smooth] probe car %08X wheel %d poly %u flags %04X h %.2f point (%.1f %.1f %.1f) normal (%.2f %.2f %.2f)\n",
                   (uint32_t)MEM_W(sp_car, (int32_t)sp), (int)MEM_W(0x14C, (int32_t)sp), (poly - polys) / poly_size, MEM_HU(0, (int32_t)poly), h0,
                   f32(rdram, pointp0), f32(rdram, pointp0 + 4), f32(rdram, pointp0 + 8),
                   f32(rdram, frame0 + 12), f32(rdram, frame0 + 16), f32(rdram, frame0 + 20));
            fflush(stdout);
        }
    }
    float fold_cos = (float)std::cos(fold_degrees(rdram) * 3.14159265358979 / 180.0);
    if (table.polys != polys || table.verts != verts || table.fold_cos != fold_cos) build(rdram, fold_cos);
    uint32_t index = (poly - polys) / poly_size;
    if (index >= table.smooth.size()) {
        ledge_clamp(rdram, ctx);
        return;
    }
    const Smooth& s = table.smooth[index];
    if (s.count == 0) {
        ledge_clamp(rdram, ctx);   // a spline or arc polygon
        return;
    }
    const Corner* c = &table.corners[s.first];

    // The wheel's point, flat on the polygon's plane (its height along the flat normal taken off), then the triangle of
    // the polygon's fan it lies in and its barycentric weights there (clamped to the triangle, so a point just outside
    // the footprint, which the probe allows, still blends its nearest edge).
    uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp);
    float p[3] = { f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8) };
    float d = (p[0] - c[0].p[0]) * s.face[0] + (p[1] - c[0].p[1]) * s.face[1] + (p[2] - c[0].p[2]) * s.face[2];
    float q[3] = { p[0] - d * s.face[0], p[1] - d * s.face[1], p[2] - d * s.face[2] };
    int best = 1;
    float bw[3] = { 1.0f, 0.0f, 0.0f }, best_out = 1e30f;
    for (int k = 1; k + 1 < s.count; k++) {
        // Barycentric weights in the plane: solve with the two edge vectors.
        float e1[3], e2[3], r[3];
        for (int a = 0; a < 3; a++) {
            e1[a] = c[k].p[a] - c[0].p[a];
            e2[a] = c[k + 1].p[a] - c[0].p[a];
            r[a] = q[a] - c[0].p[a];
        }
        float d11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], d12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
        float d22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        float r1 = r[0] * e1[0] + r[1] * e1[1] + r[2] * e1[2], r2 = r[0] * e2[0] + r[1] * e2[1] + r[2] * e2[2];
        float det = d11 * d22 - d12 * d12;
        if (std::fabs(det) < 1e-9f) continue;
        float v = (d22 * r1 - d12 * r2) / det, w = (d11 * r2 - d12 * r1) / det, u = 1.0f - v - w;
        float out = std::max(0.0f, -u) + std::max(0.0f, -v) + std::max(0.0f, -w);   // how far outside, in weights
        if (out < best_out) {
            best_out = out;
            best = k;
            bw[0] = u;
            bw[1] = v;
            bw[2] = w;
        }
        if (out == 0.0f) break;
    }
    for (int a = 0; a < 3; a++) bw[a] = std::max(0.0f, bw[a]);
    float sum = bw[0] + bw[1] + bw[2];
    if (sum <= 0.0f) {
        ledge_clamp(rdram, ctx);
        return;
    }
    for (int a = 0; a < 3; a++) bw[a] /= sum;
    const Corner* tri[3] = { &c[0], &c[best], &c[best + 1] };
    float normal[3] = {};
    for (int t = 0; t < 3; t++) {
        for (int a = 0; a < 3; a++) normal[a] += bw[t] * tri[t]->n[a];
    }
    normalize(normal);
    if (normal[0] * s.face[0] + normal[1] * s.face[1] + normal[2] * s.face[2] < 0.5f) {   // nothing sensible
        ledge_clamp(rdram, ctx);
        return;
    }

    // The frame: row 1 the smooth normal, row 0 the flat across turned onto it, row 2 closing the right-handed set
    // (checked against the flat along so the frame keeps its handedness).
    uint32_t frame = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
    float across[3] = { f32(rdram, frame), f32(rdram, frame + 4), f32(rdram, frame + 8) };
    float along_flat[3] = { f32(rdram, frame + 24), f32(rdram, frame + 28), f32(rdram, frame + 32) };
    float an = across[0] * normal[0] + across[1] * normal[1] + across[2] * normal[2];
    for (int a = 0; a < 3; a++) across[a] -= an * normal[a];
    normalize(across);
    float along[3] = { across[1] * normal[2] - across[2] * normal[1], across[2] * normal[0] - across[0] * normal[2],
                       across[0] * normal[1] - across[1] * normal[0] };
    if (along[0] * along_flat[0] + along[1] * along_flat[1] + along[2] * along_flat[2] < 0.0f) {
        for (int a = 0; a < 3; a++) along[a] = -along[a];
    }
    for (int a = 0; a < 3; a++) {
        put_f32(rdram, frame + 4 * a, across[a]);
        put_f32(rdram, frame + 12 + 4 * a, normal[a]);
        put_f32(rdram, frame + 24 + 4 * a, along[a]);
    }
    // The height: the wheel's distance to the flat face along the smooth normal.
    float h = (p[0] - q[0]) * normal[0] + (p[1] - q[1]) * normal[1] + (p[2] - q[2]) * normal[2];
    // A ledge is climbed, not jumped onto (a landing keeps its depth).
    put_f32(rdram, (uint32_t)ctx->r17 + 4, h < -ledge_step && !falling(rdram, sp) ? -ledge_rise : h);
}

// Debug (RUSH2_SMOOTH_LOG=2): car_ground_contact_800703EC at 0x8007057C, right after a wheel's probe: $s1 = the car,
// $s3 = the wheel, 0xB8($sp) = the height the probe returned, +0x5CC the wheel's last compression. Every probe of the
// player's car whose height is more than half a foot off is printed (a kick in the making, or a missed floor).
extern "C" void rush2_smooth_debug_contact(uint8_t* rdram, recomp_context* ctx) {
    if (!log_probes) return;
    uint32_t car = (uint32_t)ctx->r17;
    if (car != player_car) return;
    float h = f32(rdram, (uint32_t)ctx->r29 + 0xB8);
    if (h > -0.5f && h < 0.5f) return;
    int wheel = (int)ctx->r19;
    uint32_t pointp = car + 0x230 + 12 * (uint32_t)wheel;
    printf("[smooth] contact wheel %d h %.2f last %.2f type %d point (%.1f %.1f %.1f) normal (%.2f %.2f %.2f)\n", wheel, h,
           f32(rdram, car + 0x5CC + 4 * (uint32_t)wheel), (int)MEM_W(0x5FC + 4 * wheel, (int32_t)car),
           f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8),
           f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 12), f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 16),
           f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 20));
    fflush(stdout);
}

int rush2::collision::polygon_count(uint8_t* rdram) {
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
    if (polys == 0 || verts <= polys) return 0;
    return (int)((verts - polys) / poly_size);
}

int rush2::collision::polygon(uint8_t* rdram, int i, uint16_t& flags, float out[16][3]) {
    uint32_t poly = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr) + poly_size * (uint32_t)i;
    flags = MEM_HU(0, (int32_t)poly);
    float m[9];
    return world_vertices(rdram, poly, out, m);
}
