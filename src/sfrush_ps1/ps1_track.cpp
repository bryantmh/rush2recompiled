// Converts the PlayStation SF Rush's own track (track 4: Presidio, Chinatown, the Financial District) into a custom
// track (docs/custom_tracks.md) at the disc's full detail. Formats: docs/sfrush_ps1.md; the Python readers in
// tools/sfrush_ps1/ (track.py, vram.py, collision.py) are the research versions. tracks/chinatown/track.glb holds:
//   - scenery: the road and scenery chunks (T4ROAD2, T4SCN2), one material per texture page and CLUT, sampled from a
//     copy of the PS1's VRAM laid out as the game's loaders lay it out
//   - COL_*: the collision map's ground polygons by surface (FLAGS & 0x18), its walls as vertical faces
//   - FINISH and CHECKPOINT_1-3 where TRACK4.CPF's planes cross the AI path
//   - AIPATH_main between the drones' two lines (PATHDAT4 and EXTDAT4, the corridor the PS1 drones drive across) and
//     AIPATH_lane1-4 across that corridor, as the PS1 drones pick their line (a lerp of the two by car)
//   - PROP_KNOCK_cone_*: T4KNOCK's cones where 0x800A58B4 puts them; MOVE_tram_*: its trams, running back and forth
//     as the PS1 runs them, each with a COL_WALL_ box
// Left out: the PS1's race times (the game works them out from the AI path).

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "sfrush_ps1.h"
#include "texture_upscale.h"
#include "../arcade2049/arcade_internal.h"

namespace ps1 = rush2::sfrush_ps1;
namespace a49 = rush2::arcade2049;
using namespace a49;   // V3 and its operators

namespace {
    // The PS1 races each track in three conditions over the same road, scenery and collision (docs/sfrush_ps1.md#snow-
    // and-night): day, snow (gp+0x40, 0x800B2710 == 1) and night (gp+0x98, 0x800B2768 set). Each is a custom track of
    // its own (docs/custom_tracks.md#versions): track 4, the PS1's own course, by day (Chinatown) with its snow and night
    // versions; tracks 1-3, the N64 game's first three courses in the PS1's own models, as snow and night versions of
    // the N64 courses (their day is the N64 version's).
    enum class Condition { day, snow, night };
    struct ConditionInfo {
        Condition condition;
        const char* suffix;      // T<n>PAGE<suffix>.DAT, T<n>CLUT<suffix>.DAT, the folder's _snow / _night
        const char* variant;     // the version button's label
    };
    constexpr ConditionInfo conditions[] = {
        { Condition::day, "", "DAY" },
        { Condition::snow, "S", "SNOW" },
        { Condition::night, "N", "NIGHT" },
    };

    // What differs between the PS1's four tracks in RUSH.EXE, by track index (0x800B34F8 / gp+0x70: 0-3).
    struct TrackInfo {
        int n;                      // the PS1's track number, T<n>... and COLLDAT<n>
        const char* id;             // the day version's folder (and the assets' names); others add _snow / _night
        const char* name;
        const char* course;         // the N64 course the versions are of ([menu] variant_of), nullptr: the day track
        bool day;                   // a day version of its own (track 4: the N64 game hasn't the course)
        uint32_t page_loader, clut_loader;   // docs/sfrush_ps1.md#textures
        uint32_t tex_table;                  // func_800369B8: 0x800A7478 for track index 1, 0x800A77C0 for the others
        uint32_t banner_id_insn, banner_alt_insn;   // track_draw_chunks' finish banner swap (addiu id, lw the other)
        uint32_t gate_counts, gate_sectors, gate_first, gate_times;   // checkpoint_test / checkpoint_passed's tables
        uint32_t start_insn;        // the race setup's forward start time (0x80015D60: addiu $v0, seconds)
        uint32_t bonus_insn;        // checkpoint_passed's extra on the first two gates (track 4 forward), 0: none
        uint32_t sections;          // track_draw_chunks' section table (32 bytes each: +0x18, +0x1C visible lists)
        std::array<int, 2> semi;    // chunk ids the draw loop draws semi-transparent (gp+0x310), 0: none
        uint32_t prop_table;        // props_place's records (func_800155F8), 0: none
        // A run-in circuit's grid (docs/sfrush_ps1.md#run-in): the race setup's forward grid instruction (ori, the back
        // car's PS1 y in 0.1 ft) for a track whose AI path is open, its race starting on a run-in to the loop; 0: a loop.
        uint32_t run_in_grid;
    };
    constexpr TrackInfo tracks[] = {
        { 1, "golden_gate", "Golden Gate", "sfrush:golden_gate", false, 0x80037C2C, 0x8003A4A0, 0x800A77C0, 0x80044524,
          0x80044534, 0x800B2AF8, 0x800AA388, 0x800B2B00, 0x800B2AD8, 0x80015DF0, 0, 0x8009DEF4, { 0x70, 0x72 }, 0,
          0x80051770 },
        { 2, "embarcadero", "Embarcadero", "sfrush:embarcadero", false, 0x80038644, 0x8003AF18, 0x800A7478, 0x8004459C,
          0x800445AC, 0x800B2B08, 0x800AA394, 0x800B2B10, 0x800B2AE0, 0x80015DCC, 0, 0x800A0180, { 0x82, 0x83 }, 0x800A55B4,
          0 },
        // Track 3's gate sectors forward (0x800AA3AC backward, gp+0x4C set).
        { 3, "market", "Market", "sfrush:market", false, 0x80039048, 0x8003B990, 0x800A77C0, 0x800445DC, 0x800445EC,
          0x800AA3A0, 0x800AA3BC, 0x800AA3CC, 0x800AA37C, 0x80015DA8, 0, 0x800A2798, { 0, 0 }, 0, 0 },
        { 4, "chinatown", "Chinatown", nullptr, true, 0x80039A88, 0x8003C408, 0x800A77C0, 0x80044628, 0x80044638,
          0x800B2B18, 0x800B2B1C, 0x800AA3D8, 0x800B2AE8, 0x80015D80, 0x800542D8, 0x800A4188, { 0x59, 0 }, 0x800A58B4, 0 },
    };

    // The computer cars' speeds ([ai] speed_quantiles, docs/custom_tracks.md#ai-speeds): the PS1 has no AI speed data
    // the converter reads, and the stock radius table had them 10-30 mph slow in Golden Gate's corners (7 drones over
    // 90 s: N64 118 mph, PS1 night 107, PS1 snow 91; user report 2026-11-09). Tracks 1-3 take their N64 versions'
    // lane speeds (N64 tracks 1-3, assets 58-60), track 4, a course the N64 lacks, all seven N64 tracks':
    // tools/rush1/lane_speed_table.py --tracks 0 / 1 / 2 / 0,1,2,3,4,5,6. Snow slows the tight corners on top
    // ([handling] traction).
    std::string ai_speed_toml(int track) {
        static const char* const quantiles[] = {
            "69, 127, 134, 134, 139, 140, 141, 144, 147, 149, 151, 151, 153, 154, 155, 157, 158, 158, 159, 161, 174",
            "103, 117, 122, 128, 132, 134, 136, 138, 140, 142, 144, 145, 145, 147, 147, 148, 151, 151, 153, 154, 158",
            "70, 87, 95, 100, 106, 110, 114, 117, 119, 121, 123, 125, 128, 131, 134, 136, 141, 146, 173, 174, 174",
            "70, 91, 99, 105, 109, 114, 119, 123, 128, 133, 136, 138, 141, 145, 147, 150, 153, 155, 158, 161, 174",
        };
        if (track < 1 || track > 4) return "";
        return std::string("[ai]\nspeed_quantiles = [") + quantiles[track - 1] + "]\n\n";
    }
    // In snow the race setup sets the clock to 60 whatever the track (0x80015E04, gp+0x40 == 1).
    constexpr uint32_t snow_start_insn = 0x80015E04;

    // RUSH.EXE (docs/sfrush_ps1.md#textures).
    constexpr uint32_t exe_base = ps1::Exe::base;
    constexpr uint32_t load_image = 0x80092C58, load_clut = 0x80014F38;
    constexpr int tex_entries = 35;
    constexpr uint32_t clut_ids = 0x800A7B08;
    constexpr int clut_id_count = 64;

    int16_t s16(const uint8_t* p) { return int16_t(p[0] | p[1] << 8); }
    uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    int32_t s32(const uint8_t* p) { return int32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

    using Exe = ps1::Exe;

    // The page loader's LoadImage RECTs and the CLUT loader's (x, y), in call order: the loaders are straight-line
    // code, so a scan that follows constant registers and the RECT on the stack finds them (vram.py scan). Each call
    // gets the .DAT chunk the loader's chunk pointer is at: it steps by the .SIZ sizes (lw rt, 4k($s1) adds chunk k's),
    // so after chunk k's size the next call gets chunk k + 1. Track 2's CLUT loader (func_8003AF18) steps over chunks
    // 10 and 26 without uploading them, so its 50 calls aren't the file's first 50 chunks (docs/sfrush_ps1.md#textures).
    struct Calls {
        std::vector<std::array<int, 4>> images;
        std::vector<std::array<int, 2>> cluts;
        std::vector<int> image_chunks, clut_chunks;
    };
    void scan(const Exe& e, uint32_t start, Calls& out) {
        std::map<int, uint32_t> regs{ { 0, 0 } };
        std::map<int, uint32_t> stack;
        int chunk = 0;
        uint32_t end = e.func_end(start);
        auto apply = [&](uint32_t w) {
            int op = int(w >> 26), rs = int((w >> 21) & 31), rt = int((w >> 16) & 31);
            uint32_t imm = w & 0xFFFF;
            int32_t simm = int16_t(imm);
            if (op == 9 && regs.count(rs)) regs[rt] = regs[rs] + uint32_t(simm);
            else if (op == 0xD && regs.count(rs)) regs[rt] = regs[rs] | imm;
            else if (op == 0xF) regs[rt] = imm << 16;
            else if (op == 0x29 && rs == 29 && regs.count(rt)) stack[simm] = regs[rt] & 0xFFFF;
            else if (op == 0x23 && rs == 17) {   // lw rt, 4k($s1): chunk k's size, added to the chunk pointer
                chunk = simm / 4 + 1;
                regs.erase(rt);
            }
            else if (op == 0) {
                int rd = int((w >> 11) & 31), fn = int(w & 0x3F);
                if ((fn == 0x21 || fn == 0x25) && rt == 0 && regs.count(rs)) regs[rd] = regs[rs];
                else regs.erase(rd);
            } else if (op != 1 && (op < 2 || op > 7) && op != 0x28 && op != 0x29 && op != 0x2B) regs.erase(rt);
            regs[0] = 0;
        };
        for (uint32_t a = start; a < end && e.in(a, 8);) {
            uint32_t w = e.w(a);
            if ((w >> 26) == 3) {
                uint32_t t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
                apply(e.w(a + 4));   // the delay slot runs first
                auto st = [&](int o) { auto it = stack.find(o); return it == stack.end() ? -1 : int(it->second); };
                if (t == load_image) {
                    out.images.push_back({ st(0x10), st(0x12), st(0x14), st(0x16) });
                    out.image_chunks.push_back(chunk);
                }
                else if (t == load_clut) {
                    out.cluts.push_back({ regs.count(4) ? int(regs[4]) : -1, regs.count(5) ? int(regs[5]) : -1 });
                    out.clut_chunks.push_back(chunk);
                }
                for (int r : { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25 }) regs.erase(r);
                a += 8;
                continue;
            }
            apply(w);
            a += 4;
        }
    }

    // 1024 x 512 halfwords, the track's pages and CLUTs uploaded as the game does (vram.py Vram).
    struct Vram {
        std::vector<uint16_t> v = std::vector<uint16_t>(1024 * 512, 0);
        std::array<uint8_t, 4> texel(int tx, int ty, int clut, int u, int vv) const {
            uint16_t h = v[size_t((ty + (vv & 255)) & 511) * 1024 + size_t((tx + ((u & 255) >> 2)) & 1023)];
            int i = (h >> ((u & 3) * 4)) & 15;
            int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
            uint16_t c = v[size_t(cy) * 1024 + size_t(cx + i)];
            uint8_t r = uint8_t((c & 31) << 3), g = uint8_t(((c >> 5) & 31) << 3), b = uint8_t(((c >> 10) & 31) << 3);
            // 0x0000 is the PS1's transparent texel; any other value (black 0x8000 too) is drawn.
            return { uint8_t(r | r >> 5), uint8_t(g | g >> 5), uint8_t(b | b >> 5), uint8_t(c == 0 ? 0 : 255) };
        }
    };

    // PS1 track frame (ft: x, y across the ground, z up) to the game's (docs/sfrush_ps1.md#units-and-axes). This frame
    // is the PS1 world's mirror image; gl() writes it to glTF un-mirrored (docs/sfrush_ps1.md#handedness).
    V3 game(double x, double y, double z) { return { y, z, x }; }
    // A game frame point (ft) in glTF (meters) as the PS1 shows it: a49::to_gltf without its x negation, so the
    // world isn't the PS1's mirror image (the McDouglas Tunnel / Freeway signs read right only so). Every winding
    // written with it is the one the game frame has.
    V3 gl(const V3& p) { V3 q = a49::to_gltf(p); q[0] = -q[0]; return q; }
    // A game frame point mirrored so a49 helpers that apply to_gltf themselves (gate_matrix) write it as gl() does.
    V3 for_a49(const V3& p) { return { -p[0], p[1], p[2] }; }
    // Collision and AI path units: 0.1 ft, (x, -height, y).
    V3 game_coll(int32_t a, int32_t b, int32_t c) { return game(a / 10.0, c / 10.0, -b / 10.0); }

    struct Face {
        uint32_t flags;
        int clut;
        std::array<uint16_t, 4> idx;
        std::array<std::array<int, 2>, 4> uv;   // (u, v)
        uint32_t tex;
    };
    struct Chunk {
        std::vector<V3> verts;   // game ft
        std::vector<Face> faces;
    };
    bool read_chunk(const std::vector<uint8_t>& c, Chunk& out) {
        if (c.size() < 0x20) return false;
        int32_t ox = s32(&c[4]), oy = s32(&c[8]), oz = s32(&c[12]);
        uint32_t nv = uint32_t(s32(&c[0x10])), nf = uint32_t(s32(&c[0x14]));
        if (0x20 + size_t(nv) * 12 + size_t(nf) * 0x28 != c.size()) return false;
        for (uint32_t i = 0; i < nv; i++) {
            const uint8_t* p = &c[0x20 + 12 * i];
            out.verts.push_back(game(ox / 256.0 + s32(p) / 65536.0, oy / 256.0 + s32(p + 4) / 65536.0,
                                     oz / 256.0 + s32(p + 8) / 65536.0));
        }
        size_t fo = 0x20 + size_t(nv) * 12;
        for (uint32_t i = 0; i < nf; i++) {
            const uint8_t* f = &c[fo + 0x28 * i];
            Face face;
            face.flags = f[0] | f[1] << 8 | f[2] << 16;
            face.clut = int8_t(f[3]);
            for (int k = 0; k < 4; k++) face.idx[k] = u16(f + 4 + 2 * k);
            // Stored v, then u. The renderer (func_80069684) gives vertex 0 corner 1's coordinate, vertex 1 corner 0's,
            // vertex 2 corner 3's and vertex 3 corner 2's (its POLY_FT4 takes the vertices 0, 1, 3, 2 and the UVs
            // 1, 0, 2, 3), so each vertex here gets its pair's.
            for (int k = 0; k < 4; k++) face.uv[k ^ 1] = { u16(f + 0xE + 4 * k), u16(f + 0xC + 4 * k) };
            face.tex = uint32_t(s32(f + 0x1C));
            out.faces.push_back(face);
        }
        return true;
    }

    std::vector<std::array<uint32_t, 3>> fan(size_t n, uint32_t base) {
        std::vector<std::array<uint32_t, 3>> t;
        // The PS1's ring is clockwise seen from the front in the game's frame, and gl() keeps that frame's turn, so the
        // fan goes backwards to be counter-clockwise, as glTF's front faces are (docs/sfrush_ps1.md#track-geometry).
        for (uint32_t a = 1; a + 1 < n; a++) t.push_back({ base, base + a + 1, base + a });
        return t;
    }

    struct Ground {
        std::array<V3, 4> p;     // game ft
        int n;                   // 3 or 4 corners
        uint8_t flags;
        std::array<std::array<int, 3>, 2> tris;   // into p
        int tri_count;
    };
    struct Wall {
        V2 a, b;                 // game (x, z) ft
        V2 normal;               // game (x, z), into the road
        int chunk;
        double low = 0, high = 0;   // a raised wall's (B record's) height band, game y ft; 0, 0 for an A wall
    };

    // ------------------------------------------------------------------------------------------- spline road
    // The PS1's ground is flat planes (docs/sfrush_ps1.md#collision), and on tracks 1-3 most are bent quads: Golden
    // Gate's are 583 of 859, folded a median 2 degrees along a diagonal, so the road's bank wobbles from quad to quad
    // every 40-100 ft and every edge is a crease (Chinatown's are 37 of 922). The PS1 hides it by damping its cars'
    // pitch and roll hard every frame on the ground (car_damp_x, car_damp_z); Rush 2 rides it, and Golden Gate rocked
    // and bumped all round (user report 2026-11-09; measured with tools/test_race.py --motion: roll jerk 9x the N64
    // Golden Gate's). Rush 2's own tracks smooth their road with the spline road (docs/custom_tracks.md#spline-road),
    // so the converter fits one to the PS1 road: stations every spline_spacing ft along the drones' line, at each a
    // profile across the road (heights every strip_width ft, least squares through the PS1 ground under it), smoothed
    // along the lap. A segment's cross-section is a straight line, and the PS1's turns are dished (flat in the middle,
    // rising 6-8 ft toward the edges: one straight line left most of a turn 1-8 ft off and flat), so the road is
    // strip_count parallel chains, one per strip between two knots, meeting at the knots. a49::cut_spline_road (the
    // arcade converter's) cuts the floor under each strip's segments into footprints that take their height from
    // them, keeping a piece only where its facet is within 1 ft of the fitted surface; kerbs, walls' feet and the rest
    // stay flat.
    // ft between stations (at 50, the foot of Golden Gate's run-in ramp fell outside the stations' profiles and
    // stayed flat: pitch jerk 0.28 -> 0.41, the worst 4.1 -> 25.7)
    constexpr double spline_spacing = 40.0;
    constexpr int strip_count = 10;           // strips across the road
    constexpr double strip_width = 12.0;      // ft
    constexpr double strip_reach = 0.5 * strip_count * strip_width;   // ft each side of the drones' line
    // The tracks it is fitted to, measured in game (tools/test_race.py --motion 1, a computer car's lap: pitch and roll
    // jerk, 95th percentile, rad/s^2) against the faces with the averaged height ([track] smooth_height) and the N64
    // course: Golden Gate 1.71 / 1.01 -> 0.28 / 0.29 (N64 0.74 / 0.11). Market rode smoothly already (1.90 / 0.86,
    // N64 2.59 / 4.16), and its jumps' crests and its size (at 40 ft its collision passed the game's 64 KB of vertex
    // lists) went worse with it (5.08 / 4.37 at 50 ft): it keeps the faces. Embarcadero (4.82 / 4.42, N64 1.45 /
    // 0.37) shudders side to side every 5 ft along its waterfront straight (2,900-4,300 ft into the lap) with or
    // without it (6.08 / 6.17 with), and the strips left that straight flat: not this, not fitted yet. Chinatown's floor
    // is flat quads.
    bool spline_road_track(int n) { return n == 1; }
    struct GroundTri {
        std::array<V3, 3> p;     // game ft (this file's frame: the PS1's mirror image, see game())
        V3 n;                    // unit, up
        int cls;                 // 0 road, 1 dirt, 2 verge
    };
    // The segments' SEG records (game space: for_a49), chain by chain (strip i's station j at i x per_chain + j), each
    // one's band (u range), and per ground triangle the segment it lies on (-1: none).
    struct RoadSplines {
        std::vector<std::array<double, 33>> records;
        std::vector<std::array<double, 2>> u_band;
        std::vector<int> hint;
        int per_chain = 0, stations = 0;
        bool open = false;
        int weak = 0;            // stations whose fit fell back on the ones before (too little ground under them)
    };
    RoadSplines road_splines(const std::vector<GroundTri>& tris, const std::vector<V3>& mid, const std::vector<V3>& lp,
                             const std::vector<V3>& rp, bool open) {
        RoadSplines out;
        out.hint.assign(tris.size(), -1);
        size_t m = mid.size();
        if (m < 4) return out;
        // The ground triangles by 32 ft cell, and the height of the one under (x, z) nearest y_ref, within tol.
        constexpr double cell = 32.0;
        std::map<std::pair<int, int>, std::vector<int>> cells;
        for (size_t i = 0; i < tris.size(); i++) {
            const auto& p = tris[i].p;
            double x0 = std::min({ p[0][0], p[1][0], p[2][0] }), x1 = std::max({ p[0][0], p[1][0], p[2][0] });
            double z0 = std::min({ p[0][2], p[1][2], p[2][2] }), z1 = std::max({ p[0][2], p[1][2], p[2][2] });
            for (int gx = int(std::floor(x0 / cell)); gx <= int(std::floor(x1 / cell)); gx++)
                for (int gz = int(std::floor(z0 / cell)); gz <= int(std::floor(z1 / cell)); gz++) cells[{ gx, gz }].push_back(int(i));
        }
        auto height_at = [&](double x, double z, double y_ref, double tol, double& y) {
            auto it = cells.find({ int(std::floor(x / cell)), int(std::floor(z / cell)) });
            if (it == cells.end()) return false;
            double best = tol;
            bool found = false;
            for (int i : it->second) {
                const auto& p = tris[i].p;
                auto side = [&](const V3& a, const V3& b) { return (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]); };
                double s0 = side(p[0], p[1]), s1 = side(p[1], p[2]), s2 = side(p[2], p[0]);
                if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
                const V3& n = tris[i].n;
                double h = p[0][1] - (n[0] * (x - p[0][0]) + n[2] * (z - p[0][2])) / n[1];
                if (std::fabs(h - y_ref) < best) {
                    best = std::fabs(h - y_ref);
                    y = h;
                    found = true;
                }
            }
            return found;
        };

        // Cross-section stations: evenly along the line (horizontal arc length; a loop wraps), the drones' corridor
        // half-width there, then the line smoothed (three [1 2 1] passes) so the stations turn gradually.
        std::vector<double> at(m + 1, 0.0);
        size_t links = open ? m - 1 : m;
        for (size_t i = 0; i < links; i++) {
            const V3 &a = mid[i], &b = mid[(i + 1) % m];
            at[i + 1] = at[i] + std::hypot(b[0] - a[0], b[2] - a[2]);
        }
        double total = at[links];
        int ns = std::max(4, int(std::lround(total / spline_spacing))) + (open ? 1 : 0);
        double step = total / (open ? ns - 1 : ns);
        std::vector<V3> P(ns);
        std::vector<double> half(ns);
        for (int j = 0; j < ns; j++) {
            double s = std::min(step * j, total);
            size_t i = size_t(std::upper_bound(at.begin(), at.begin() + links + 1, s) - at.begin());
            i = std::clamp<size_t>(i, 1, links) - 1;
            double f = at[i + 1] > at[i] ? (s - at[i]) / (at[i + 1] - at[i]) : 0.0;
            size_t k = (i + 1) % m;
            P[j] = mid[i] + (mid[k] - mid[i]) * f;
            half[j] = 0.5 * std::hypot(lp[i][0] - rp[i][0] + (lp[k][0] - rp[k][0] - lp[i][0] + rp[i][0]) * f,
                                       lp[i][2] - rp[i][2] + (lp[k][2] - rp[k][2] - lp[i][2] + rp[i][2]) * f);
        }
        auto idx = [&](int j) { return open ? std::clamp(j, 0, ns - 1) : (j % ns + ns) % ns; };
        for (int pass = 0; pass < 3; pass++) {
            std::vector<V3> q = P;
            for (int j = 0; j < ns; j++) {
                if (open && (j == 0 || j == ns - 1)) continue;
                for (int c : { 0, 2 }) q[j][c] = 0.25 * P[idx(j - 1)][c] + 0.5 * P[j][c] + 0.25 * P[idx(j + 1)][c];
            }
            P = q;
        }
        std::vector<V3> T(ns), R(ns);
        for (int j = 0; j < ns; j++) {
            V3 d = P[idx(j + 1)] - P[idx(j - 1)];
            double l = std::hypot(d[0], d[2]);
            T[j] = l > 0 ? V3{ d[0] / l, 0.0, d[2] / l } : V3{ 0.0, 0.0, 1.0 };
            R[j] = { T[j][2], 0.0, -T[j][0] };
        }

        // Each station's profile across the road: heights at knots every strip_width ft from -strip_reach to
        // +strip_reach, least squares through the ground under it (every 2 ft), lightly held to a straight line where
        // the ground gives out (a gap, the road's edge), points over 1 ft off dropped and fitted again. The ground is
        // taken nearest the profile expected from the stations before (so a deck over a road keeps to its own level);
        // failing that, the drones' line's own height there, nearer then wider.
        const int nk = strip_count + 1;
        auto knot = [&](int i) { return -strip_reach + strip_width * i; };
        std::vector<std::vector<double>> Y(ns, std::vector<double>(nk, 0.0));
        auto fit = [&](int j, const std::vector<double>& expect, double tol, std::vector<double>& y) {
            std::vector<std::array<double, 2>> pts;
            for (double s = -strip_reach; s <= strip_reach + 1e-9; s += 2.0) {
                int i = std::clamp(int((s + strip_reach) / strip_width), 0, strip_count - 1);
                double f = (s - knot(i)) / strip_width, e = expect[i] * (1 - f) + expect[i + 1] * f, h;
                if (height_at(P[j][0] + R[j][0] * s, P[j][2] + R[j][2] * s, e, tol, h)) pts.push_back({ s, h });
            }
            for (int iter = 0; iter < 3; iter++) {
                if (pts.size() < 5) return false;
                // Normal equations: hat functions at the knots, plus lambda x second differences.
                std::vector<std::vector<double>> M(nk, std::vector<double>(nk + 1, 0.0));
                for (const auto& q : pts) {
                    int i = std::clamp(int((q[0] + strip_reach) / strip_width), 0, strip_count - 1);
                    double f = (q[0] - knot(i)) / strip_width, w[2] = { 1 - f, f };
                    for (int a = 0; a < 2; a++) {
                        for (int b = 0; b < 2; b++) M[i + a][i + b] += w[a] * w[b];
                        M[i + a][nk] += w[a] * q[1];
                    }
                }
                constexpr double lambda = 0.5;
                for (int i = 1; i + 1 < nk; i++) {
                    const int c[3] = { i - 1, i, i + 1 };
                    const double d[3] = { 1, -2, 1 };
                    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) M[c[a]][c[b]] += lambda * d[a] * d[b];
                }
                for (int col = 0; col < nk; col++) {     // Gaussian elimination (symmetric positive definite)
                    if (std::fabs(M[col][col]) < 1e-12) return false;
                    for (int r = col + 1; r < nk; r++) {
                        double f = M[r][col] / M[col][col];
                        for (int c = col; c <= nk; c++) M[r][c] -= f * M[col][c];
                    }
                }
                for (int r = nk - 1; r >= 0; r--) {
                    double v = M[r][nk];
                    for (int c = r + 1; c < nk; c++) v -= M[r][c] * y[c];
                    y[r] = v / M[r][r];
                }
                std::vector<std::array<double, 2>> kept;
                for (const auto& q : pts) {
                    int i = std::clamp(int((q[0] + strip_reach) / strip_width), 0, strip_count - 1);
                    double f = (q[0] - knot(i)) / strip_width;
                    if (std::fabs(y[i] * (1 - f) + y[i + 1] * f - q[1]) < 1.0) kept.push_back(q);
                }
                if (kept.size() == pts.size()) break;
                pts = std::move(kept);
            }
            return std::fabs(y[strip_count / 2] - expect[strip_count / 2]) < tol + 2.0;
        };
        for (int j = 0; j < ns; j++) {
            std::vector<double> expect(nk), y(nk);
            if (j == 0) {
                double h;
                double a = height_at(P[0][0], P[0][2], P[0][1], 80.0, h) ? h : P[0][1];
                std::fill(expect.begin(), expect.end(), a);
            } else {
                double trend = j > 1 ? Y[j - 1][strip_count / 2] - Y[j - 2][strip_count / 2] : 0.0;
                for (int i = 0; i < nk; i++) expect[i] = Y[j - 1][i] + trend;
            }
            std::vector<double> level(nk, P[j][1]);
            if (fit(j, expect, 4.0, y) || fit(j, level, 4.0, y) || fit(j, level, 15.0, y)) {
                Y[j] = y;
            } else {
                Y[j] = expect;
                out.weak++;
            }
        }
        // Smoothed along the lap ([1 4 6 4 1]): the center height once, each knot's height off the center twice (the
        // quads' twist is in the bank).
        {
            std::vector<double> c(ns);
            std::vector<std::vector<double>> d(nk, std::vector<double>(ns));
            for (int j = 0; j < ns; j++) {
                c[j] = Y[j][strip_count / 2];
                for (int i = 0; i < nk; i++) d[i][j] = Y[j][i] - c[j];
            }
            auto smooth = [&](std::vector<double>& v) {
                std::vector<double> w = v;
                for (int j = 0; j < ns; j++) {
                    if (open && (j < 2 || j > ns - 3)) continue;
                    w[j] = (v[idx(j - 2)] + 4 * v[idx(j - 1)] + 6 * v[j] + 4 * v[idx(j + 1)] + v[idx(j + 2)]) / 16.0;
                }
                v = w;
            };
            // (Stiffer: twice for the center, three times for the knots, or lambda 2-6 below, left more of the road
            // over 1 ft off its fit and flat, and Golden Gate's roll and pitch jerk went up 20-130%.)
            smooth(c);
            for (int i = 0; i < nk; i++) {
                smooth(d[i]);
                smooth(d[i]);
            }
            for (int j = 0; j < ns; j++) for (int i = 0; i < nk; i++) Y[j][i] = c[j] + d[i][j];
        }

        // The SEG records, in game space (for_a49 mirrors this file's frame), a chain per strip (fields:
        // docs/custom_tracks.md#spline-road). A strip's cross-section at a station is the straight line between its two
        // knots. A loop's chain ends with a copy of its first station (the last segment's end line), which, like an
        // open chain's last station, has no stretch of its own (length 1e6: the cut takes it for a chain's end).
        const int per = ns + (open ? 0 : 1);
        out.records.assign(size_t(strip_count) * per, {});
        out.u_band.assign(out.records.size(), { -0.02, 1.02 });
        out.per_chain = per;
        out.stations = ns;
        out.open = open;
        for (int i = 0; i < strip_count; i++) {
            std::vector<V3> O(ns), across(ns), along(ns), up(ns);
            std::vector<double> width(ns);
            for (int j = 0; j < ns; j++) {
                V3 l = P[j] + R[j] * knot(i), r = P[j] + R[j] * knot(i + 1);
                l[1] = Y[j][i];
                r[1] = Y[j][i + 1];
                V3 lg = for_a49(l), rg = for_a49(r);
                O[j] = (lg + rg) * 0.5;
                width[j] = a49::norm(rg - lg);
                across[j] = a49::unit(rg - lg);
            }
            for (int j = 0; j < ns; j++) {
                V3 d = O[idx(j + 1)] - O[idx(j - 1)];
                V3 al = a49::unit(d - across[j] * a49::dot(d, across[j]));
                V3 u = a49::cross(al, across[j]);
                if (u[1] < 0) {
                    across[j] = across[j] * -1.0;
                    u = u * -1.0;
                }
                along[j] = al;
                up[j] = a49::unit(u);
            }
            // A segment's frame (the matrix: t and u are measured in it) lies along its chord, from its station's
            // center to the next's, so the next center is at u 0.5, t 1 exactly (the cut takes a segment whose next
            // center isn't for a chain's end: a 12 ft strip's chord in a turn strays 0.8 ft, u 0.57, off a frame
            // along the station's tangent). The surface keeps the station's own cross-section (across, the start
            // line's slant 5C in the frame) and tangent (along, which sets the normal and the cubic's end slopes).
            auto chord_frame = [&](const V3& d, int j, V3& acp, V3& upp, V3& alp) {
                alp = a49::unit(d);
                acp = a49::unit(across[j] - alp * a49::dot(across[j], alp));
                upp = a49::unit(a49::cross(alp, acp));
            };
            auto record = [&](int j, bool last) {
                std::array<double, 33> r{};
                for (int c = 0; c < 3; c++) {
                    r[c] = O[j][c];
                    r[12 + c] = across[j][c];
                    r[15 + c] = along[j][c];
                    r[18 + c] = along[j][c];
                }
                r[21] = width[j];
                V3 acp, upp, alp;
                if (last) {
                    // A chain's end: no stretch after it (length 1e6), the station's own frame; its cubic slope is
                    // the end slope of the segment before (its tangent off the chord coming in).
                    for (int c = 0; c < 3; c++) r[3 + c] = across[j][c], r[6 + c] = up[j][c], r[9 + c] = along[j][c];
                    r[22] = 1e6;
                    r[25] = r[27] = 0.5 * width[j];
                    int p = idx(j - 1);
                    if (p != j) {
                        chord_frame(O[j] - O[p], p, acp, upp, alp);
                        double fa = a49::dot(alp, along[j]);
                        r[29] = r[30] = std::fabs(fa) > 1e-6 ? a49::dot(upp, along[j]) / fa : 0.0;
                    }
                    return r;
                }
                int k = idx(j + 1);
                V3 d = O[k] - O[j];
                chord_frame(d, j, acp, upp, alp);
                for (int c = 0; c < 3; c++) r[3 + c] = acp[c], r[6 + c] = upp[c], r[9 + c] = alp[c];
                double g = a49::dot(acp, across[j]);
                double c0 = a49::dot(acp, across[k]), c2 = a49::dot(alp, across[k]);
                double s58 = a49::norm(d);
                r[22] = s58;                                                     // length at the center line
                r[23] = std::fabs(g) > 1e-6 ? a49::dot(alp, across[j]) / g : 0.0; // the start line's slant
                r[24] = std::fabs(c0) > 1e-6 ? c2 / c0 : 0.0;                     // the end line's slant
                double hw = 0.5 * width[j] * g;
                r[25] = r[27] = hw;                                              // half-widths across the frame
                r[26] = r[28] = s58 > 1e-6 ? (c0 * 0.5 * width[k] - hw) / s58 : 0.0;   // their taper along
                // The cubic's end slopes: minus the tangent's slope off the chord (so the surface leaves the station
                // along its tangent); the next segment's, at its start, ends this one the same way.
                double fa = a49::dot(alp, along[j]);
                r[29] = r[30] = std::fabs(fa) > 1e-6 ? -a49::dot(upp, along[j]) / fa : 0.0;
                return r;
            };
            for (int j = 0; j < ns; j++) out.records[size_t(i) * per + j] = record(j, open && j == ns - 1);
            if (!open) out.records[size_t(i) * per + ns] = record(0, true);
        }

        // Each ground triangle near the line is put by the nearest station whose profile it lies on (within 3 ft,
        // across within the knots) and the strip it is over; the cut tries the stations around it, every strip.
        std::map<std::pair<int, int>, std::vector<int>> by_cell;
        constexpr double scell = 64.0;
        for (int j = 0; j < ns; j++) by_cell[{ int(std::floor(P[j][0] / scell)), int(std::floor(P[j][2] / scell)) }].push_back(j);
        for (size_t t = 0; t < tris.size(); t++) {
            V3 c = (tris[t].p[0] + tris[t].p[1] + tris[t].p[2]) * (1.0 / 3.0);
            int gx = int(std::floor(c[0] / scell)), gz = int(std::floor(c[2] / scell));
            double best = 1e18;
            for (int dx = -2; dx <= 2; dx++) {
                for (int dz = -2; dz <= 2; dz++) {
                    auto it = by_cell.find({ gx + dx, gz + dz });
                    if (it == by_cell.end()) continue;
                    for (int j : it->second) {
                        V3 d = c - P[j];
                        double lat = d[0] * R[j][0] + d[2] * R[j][2], lon = d[0] * T[j][0] + d[2] * T[j][2];
                        if (std::fabs(lat) > strip_reach) continue;
                        int i = std::clamp(int((lat + strip_reach) / strip_width), 0, strip_count - 1);
                        double f = (lat - knot(i)) / strip_width;
                        auto prof = [&](int jj) { return Y[jj][i] * (1 - f) + Y[jj][i + 1] * f; };
                        // The profile carried along the road's slope to the triangle (the run-in's ramp off the
                        // bridge falls 25%: 10 ft in 40).
                        int ja = idx(j - 1), jb = idx(j + 1);
                        double run = std::hypot(P[jb][0] - P[ja][0], P[jb][2] - P[ja][2]);
                        double slope = run > 1e-6 ? (prof(jb) - prof(ja)) / run : 0.0;
                        if (std::fabs(c[1] - (prof(j) + slope * lon)) > 3.0) continue;
                        double dist = lat * lat + lon * lon;
                        if (dist < best) {
                            best = dist;
                            out.hint[t] = i * per + j;
                        }
                    }
                }
            }
        }
        return out;
    }
}

static bool build_condition(const ps1::Disc& disc, const std::filesystem::path& assets_dir, ps1::ModFiles& out,
                            std::string& log, const TrackInfo& tr, const ConditionInfo& cond) {
    using ps1::Disc;
    char line[256];
    const char* folder_suffix = cond.condition == Condition::snow ? "_snow" : cond.condition == Condition::night ? "_night" : "";
    const char* name_suffix = cond.condition == Condition::snow ? " Snow" : cond.condition == Condition::night ? " Night" : "";
    const std::string track_id = tr.id, folder = track_id + folder_suffix, track_name = std::string(tr.name) + name_suffix;
    std::vector<std::string> report;
    Exe exe;
    // A PS1 frame in seconds: the car conversion puts the BMW's top speed (9.6 ft a frame) at Rush 2's PICKUP's
    // (239.7 ft/s, src/race_time_model.cpp's model with its values), 25 frames a second (ps1_cars.cpp ps1_physics).
    // The trams (2.4 ft a frame: 41 mph) are timed by it too.
    constexpr double frames_per_second = 239.7 / 9.6;
    if (!exe.open(disc)) {
        log += "EXE/RUSH.EXE couldn't be read\n";
        return false;
    }
    // Texture pages and CLUTs in VRAM.
    Calls pages, cluts;
    scan(exe, tr.page_loader, pages);
    scan(exe, tr.clut_loader, cluts);
    std::vector<std::vector<uint8_t>> page_chunks, clut_chunks;
    const std::string t = std::to_string(tr.n);
    const std::string sfx = cond.suffix;
    auto last = [](const std::vector<int>& v) { return v.empty() ? size_t(SIZE_MAX) : size_t(*std::max_element(v.begin(), v.end())); };
    if (!disc.chunks("T" + t + "PAGE/T" + t + "PAGE" + sfx, page_chunks) || !disc.chunks("T" + t + "CLUT/T" + t + "CLUT" + sfx, clut_chunks) ||
        last(pages.image_chunks) >= page_chunks.size() || last(cluts.clut_chunks) >= clut_chunks.size()) {
        snprintf(line, sizeof(line), "texture pages: %zu loads for %zu chunks, CLUTs: %zu for %zu\n", pages.images.size(),
                 page_chunks.size(), cluts.cluts.size(), clut_chunks.size());
        log += line;
        return false;
    }
    // The page loader (func_80039A88) loads the whole .DAT into one buffer, steps through it by the .SIZ's sizes and
    // uploads each chunk with its fixed RECT whatever the chunk's size: the night set's chunk 28 is 64 x 28 texels
    // against the RECT's 64 x 32, so its last 4 rows are read from the start of chunk 29, as here.
    std::vector<uint8_t> page_data;
    std::vector<size_t> page_at;
    for (const auto& c : page_chunks) {
        page_at.push_back(page_data.size());
        page_data.insert(page_data.end(), c.begin(), c.end());
    }
    Vram vram;
    for (size_t k = 0; k < pages.images.size(); k++) {
        auto [x, y, w, h] = pages.images[k];
        const size_t at = page_at[size_t(pages.image_chunks[k])];
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 1024 || y + h > 512 || at + 0x28 + size_t(w) * h * 2 > page_data.size()) {
            log += "a texture page's RECT is outside VRAM\n";
            return false;
        }
        const uint8_t* c = &page_data[at + 0x28];
        for (int r = 0; r < h; r++) {
            for (int i = 0; i < w; i++) vram.v[size_t(y + r) * 1024 + size_t(x + i)] = u16(&c[2 * (size_t(r) * w + i)]);
        }
    }
    // Each call's own chunk (Calls): track 2's loader skips chunks 10 and 26. Taking the calls as the chunks in order
    // gave every Embarcadero CLUT from the 11th on a later chunk's colors (noise on its walls and road, the night
    // CHECKPOINT gantry unreadable).
    for (size_t k = 0; k < cluts.cluts.size(); k++) {
        auto [x, y] = cluts.cluts[k];
        const auto& c = clut_chunks[size_t(cluts.clut_chunks[k])];
        if (x < 0 || y < 0 || x + 16 > 1024 || y >= 512 || c.size() < 40) return false;
        for (int i = 0; i < 16; i++) vram.v[size_t(y) * 1024 + size_t(x + i)] = u16(&c[8 + 2 * i]);
    }
    if (!exe.in(tr.tex_table, tex_entries * 24) || !exe.in(clut_ids, clut_id_count * 4)) return false;
    auto entry = [&](uint32_t i, int f) { return s32(&exe.data[tr.tex_table - exe_base + i * 24 + f * 4]); };
    auto clut_id = [&](int i) { return int(exe.w(clut_ids + uint32_t(i) * 4)); };

    // Scenery: the road and the scenery chunks, one material per (texture page, CLUT).
    a49::Glb g("rush2-recomp src/sfrush_ps1 (PS1 SF Rush track " + t + ")", true);
    std::map<std::array<int, 4>, int> materials;
    // semi: the chunk is drawn semi-transparent (gp+0x310: the GPU's half and half, its texels at half alpha here).
    auto material = [&](int tx, int ty, int clut, bool semi = false) {
        std::array<int, 4> key{ tx, ty, clut, semi ? 1 : 0 };
        auto it = materials.find(key);
        if (it != materials.end()) return it->second;
        std::vector<uint8_t> rgba(256 * 256 * 4);
        bool clear = false;
        for (int v = 0; v < 256; v++) {
            for (int u = 0; u < 256; u++) {
                auto c = vram.texel(tx, ty, clut, u, v);
                if (semi && c[3] != 0) c[3] = 128;
                std::memcpy(&rgba[(size_t(v) * 256 + u) * 4], c.data(), 4);
                clear = clear || c[3] == 0;
            }
        }
        snprintf(line, sizeof(line), semi ? "P%d_%d_C%d_semi" : "P%d_%d_C%d", tx, ty, clut);
        int image = g.image(line, a49::png(rgba, 256, 256));
        // Clamped: a page holds several textures, and a face's UVs never leave its own.
        int m = g.material(line, image, semi ? "BLEND" : clear ? "MASK" : "OPAQUE", false, { 33071, 33071 });
        materials[key] = m;
        return m;
    };
    size_t scenery_tris = 0, chunk_count = 0;
    // The finish banner (docs/sfrush_ps1.md#start-and-finish-banner): the scenery draw loop (func_80042FF0's chunk
    // loop at 0x80044604, track index 3) draws chunk id 2 (the gantry reading CHECKPOINT) on every lap but the viewing
    // player's last, and on that one the chunk at 0x800B5DF0 (id 90: the same gantry reading FINISH) in its place. Ids
    // index the chunk table 0x800B5C88 from 1, so they are SCN2 chunks 1 and 89. Rush 2 swaps its own finish banner the
    // same way (track_finish_line_setup_8008F080, when a player passes the last checkpoint of the last lap), so chunk 1 is the custom track's
    // FINISH_BANNER, its CHECKPOINT panels in a CHKPNT material, and chunk 89's FINISH panels are the FINISH material,
    // both resampled to one rectangle so the swap keeps the panels' coordinates (docs/custom_tracks.md#finish-banner).
    // A run-in circuit's gantry (track 1's toll plaza) is on the run-in, a checkpoint passed once: it keeps reading
    // CHECKPOINT and the FINISH chunk isn't drawn.
    const bool run_in = tr.run_in_grid != 0;
    int start_banner = -1, finish_banner = -1;
    {
        const uint32_t id_insn = tr.banner_id_insn, alt_insn = tr.banner_alt_insn, chunk_table = 0x800B5C88;
        if (exe.in(id_insn, 4) && exe.in(alt_insn, 4) && exe.w(id_insn) >> 16 == 0x2402 && exe.w(alt_insn) >> 16 == 0x8C42) {
            start_banner = int(exe.w(id_insn) & 0xFFFF) - 1;
            finish_banner = int((0x800B0000u + (exe.w(alt_insn) & 0xFFFF) - chunk_table) / 4) - 1;
        }
        else {
            report.push_back("the finish banner swap couldn't be read: both banners stay");
        }
    }
    auto face_texel = [&](const Face& f, int c) {
        return std::array<double, 2>{ double(f.uv[c][0] & 255), double((f.uv[c][1] + entry(f.tex, 3)) & 255) };
    };
    auto face_ring = [](const Face& f) {
        std::vector<int> ring;
        for (int c = 0; c < 4; c++) {
            if (ring.empty() || f.idx[c] != f.idx[ring.back()]) ring.push_back(c);
        }
        if (ring.size() == 4 && f.idx[ring[3]] == f.idx[ring[0]]) ring.pop_back();
        return ring;
    };
    // Each scenery chunk id's flags in the track's visible lists (track_draw_chunks: the section table's 32-byte records,
    // +0x18 and +0x1C lists of u32 id | flags ended by -1): 0x4000 not drawn at night, 0x2000 drawn only at night,
    // 0x1000 semi-transparent. Track 1's ids 112 and 114 are day-only, track 2's 132 and 133 night-only (and
    // semi-transparent), track 4's 89 day-only; plus the ids the draw loop itself draws semi-transparent (tr.semi).
    // The draw loop walks only these lists, so a chunk no list names is never drawn (docs/sfrush_ps1.md#unlisted-chunks):
    // Golden Gate's ids 97-100 (four chevron signs floating 100 ft over the road), 115 and 116; Embarcadero's 96,
    // 103-105, 110-114, 131 (drawn in 130's place once a car trips gp+0x88) and 134; Market's 174; Chinatown's 58 (an
    // elevated rail section). The finish banners (unlisted too) are drawn by the swap, as above.
    std::map<int, uint32_t> chunk_flags;
    std::set<int> listed;
    for (uint32_t rec = tr.sections; exe.in(rec + 0x18, 8); rec += 32) {
        uint32_t lists[2] = { exe.w(rec + 0x18), exe.w(rec + 0x1C) };
        if ((lists[0] == 0 && lists[1] == 0) || (lists[0] != 0 && !exe.in(lists[0], 4)) || (lists[1] != 0 && !exe.in(lists[1], 4))) break;
        for (uint32_t list : lists) {
            for (uint32_t a = list; list != 0 && exe.in(a, 4) && exe.w(a) != 0xFFFFFFFFu && a - list < 4096; a += 4) {
                chunk_flags[int(exe.w(a) & 0xFF)] |= exe.w(a) & 0x7000;
                listed.insert(int(exe.w(a) & 0xFF));
            }
        }
    }
    for (int id : tr.semi) if (id != 0) chunk_flags[id] |= 0x1000;
    bool banner_built = false;
    for (const char* kind : { "ROAD2", "SCN2" }) {
        std::vector<std::vector<uint8_t>> raw;
        if (!disc.chunks("T" + t + "ROAD/T" + t + kind, raw)) {
            log += std::string("T") + t + "ROAD/T" + t + kind + ".DAT couldn't be read\n";
            return false;
        }
        const bool scenery = std::string(kind) == "SCN2";
        for (size_t k = 0; k < raw.size(); k++) {
            if (scenery && (banner_built || run_in) && int(k) == finish_banner) continue;   // drawn as the FINISH material
            if (scenery && !listed.empty() && !listed.count(int(k) + 1) && int(k) != finish_banner) continue;   // never drawn
            const uint32_t cflags = scenery ? chunk_flags[int(k) + 1] : 0u;     // ids from 1
            if ((cond.condition == Condition::night && (cflags & 0x4000)) || (cond.condition != Condition::night && (cflags & 0x2000))) continue;
            const bool semi = (cflags & 0x1000) != 0;
            Chunk ch;
            if (!read_chunk(raw[k], ch)) {
                snprintf(line, sizeof(line), "%s chunk %zu doesn't add up\n", kind, k);
                log += line;
                return false;
            }
            std::map<int, a49::Glb::Prim> prims;
            // The start gantry's panels: the faces whose texture the FINISH chunk changes.
            std::set<size_t> panels;
            Chunk fin;
            if (scenery && !run_in && int(k) == start_banner && finish_banner > start_banner && size_t(finish_banner) < raw.size() &&
                read_chunk(raw[size_t(finish_banner)], fin) && fin.faces.size() == ch.faces.size()) {
                for (size_t i = 0; i < ch.faces.size(); i++) {
                    const Face& a = ch.faces[i];
                    const Face& b = fin.faces[i];
                    if (a.idx == b.idx && (a.tex != b.tex || a.uv != b.uv) && face_ring(a).size() == 4 &&
                        std::all_of(a.idx.begin(), a.idx.end(), [&](uint16_t v) { return v < ch.verts.size(); }) &&
                        a.tex < uint32_t(tex_entries) && b.tex < uint32_t(tex_entries) && a.clut >= 0 &&
                        a.clut < clut_id_count && b.clut >= 0 && b.clut < clut_id_count) {
                        panels.insert(i);
                    }
                }
            }
            if (!panels.empty()) {
                // FINISH's rectangle on its page and each image in it: FINISH as the PS1 has it, CHKPNT the CHECKPOINT
                // panel mapped corner for corner onto the same rectangle (from the first panel face; all four show it).
                std::array<double, 2> lo{ 1e9, 1e9 }, hi{ -1e9, -1e9 };
                for (size_t i : panels) {
                    for (int c = 0; c < 4; c++) {
                        auto q = face_texel(fin.faces[i], c);
                        for (int a = 0; a < 2; a++) lo[a] = std::min(lo[a], q[a]), hi[a] = std::max(hi[a], q[a]);
                    }
                }
                int w = int(hi[0] - lo[0]) + 1, h = int(hi[1] - lo[1]) + 1;
                const Face& f1 = ch.faces[*panels.begin()];
                const Face& f9 = fin.faces[*panels.begin()];
                auto page = [&](const Face& f) {
                    return std::array<int, 3>{ entry(f.tex, 0) & ~63, entry(f.tex, 1) & 256, clut_id(f.clut) };
                };
                std::vector<uint8_t> chk(size_t(w) * h * 4), fn(size_t(w) * h * 4);
                bool chk_clear = false, fn_clear = false;
                auto p1 = page(f1), p9 = page(f9);
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        double s = w > 1 ? double(x) / (w - 1) : 0.0, tt = h > 1 ? double(y) / (h - 1) : 0.0;
                        std::array<double, 2> src{};
                        for (int c = 0; c < 4; c++) {
                            auto n = face_texel(f9, c);
                            double wc = (n[0] > lo[0] ? s : 1.0 - s) * (n[1] > lo[1] ? tt : 1.0 - tt);
                            auto q = face_texel(f1, c);
                            src[0] += wc * q[0], src[1] += wc * q[1];
                        }
                        auto c1 = vram.texel(p1[0], p1[1], p1[2], int(std::lround(src[0])) & 255, int(std::lround(src[1])) & 255);
                        auto c9 = vram.texel(p9[0], p9[1], p9[2], (int(lo[0]) + x) & 255, (int(lo[1]) + y) & 255);
                        std::memcpy(&chk[(size_t(y) * w + x) * 4], c1.data(), 4);
                        std::memcpy(&fn[(size_t(y) * w + x) * 4], c9.data(), 4);
                        chk_clear = chk_clear || c1[3] == 0;
                        fn_clear = fn_clear || c9[3] == 0;
                    }
                }
                int chk_material = g.material("CHKPNT", g.image("CHKPNT", a49::png(chk, w, h)), chk_clear ? "MASK" : "OPAQUE",
                                              false, { 33071, 33071 });
                g.material("FINISH", g.image("FINISH", a49::png(fn, w, h)), fn_clear ? "MASK" : "OPAQUE", false, { 33071, 33071 });
                a49::Glb::Prim& p = prims[-1 - chk_material];
                p.material = chk_material;
                for (size_t i : panels) {
                    const Face& a = ch.faces[i];
                    const Face& b = fin.faces[i];
                    uint32_t base = uint32_t(p.verts.size());
                    std::array<double, 2> centre{};
                    for (int c = 0; c < 4; c++) {
                        auto q = face_texel(b, c);
                        centre[0] += (q[0] - lo[0]) / 4.0, centre[1] += (q[1] - lo[1]) / 4.0;
                    }
                    for (int c = 0; c < 4; c++) {
                        auto q = face_texel(b, c);
                        p.verts.push_back(gl(ch.verts[a.idx[c]]));
                        std::array<double, 2> uv = ps1::texel_inset({ q[0] - lo[0], q[1] - lo[1] }, centre);
                        p.uvs.push_back({ uv[0] / w, uv[1] / h });
                    }
                    auto tris = fan(4, base);
                    V3 n = a49::unit(a49::cross(p.verts[tris[0][1]] - p.verts[tris[0][0]], p.verts[tris[0][2]] - p.verts[tris[0][0]]));
                    for (int c = 0; c < 4; c++) p.normals.push_back(n);
                    for (auto& tri : tris) p.tris.push_back(tri);
                    scenery_tris += 2;
                }
            }
            for (size_t fi = 0; fi < ch.faces.size(); fi++) {
                const Face& f = ch.faces[fi];
                if (panels.count(fi)) continue;
                if (f.tex >= uint32_t(tex_entries) || f.clut < 0 || f.clut >= clut_id_count) continue;
                int tx = entry(f.tex, 0), ty = entry(f.tex, 1);
                int m = material(tx & ~63, ty & 256, clut_id(f.clut), semi);
                a49::Glb::Prim& p = prims[m];
                p.material = m;
                // A ring of 3 or 4 corners (a triangle repeats a corner).
                std::vector<int> ring = face_ring(f);
                if (ring.size() < 3) continue;
                if (std::any_of(ring.begin(), ring.end(), [&](int c) { return f.idx[c] >= ch.verts.size(); })) continue;
                uint32_t base = uint32_t(p.verts.size());
                // The draw record keeps a corner's u and v + the entry's v offset as bytes (track_chunk_fixup), so the
                // GPU samples them modulo 256.
                std::array<double, 2> centre{};
                for (int c : ring) centre[0] += face_texel(f, c)[0] / double(ring.size()), centre[1] += face_texel(f, c)[1] / double(ring.size());
                for (int c : ring) {
                    p.verts.push_back(gl(ch.verts[f.idx[c]]));
                    std::array<double, 2> uv = ps1::texel_inset(face_texel(f, c), centre);
                    p.uvs.push_back({ uv[0] / 256.0, uv[1] / 256.0 });
                }
                auto tris = fan(ring.size(), base);
                // The normal of the face as the glTF winds it (front faces counter-clockwise).
                V3 n = a49::unit(a49::cross(p.verts[tris[0][1]] - p.verts[tris[0][0]], p.verts[tris[0][2]] - p.verts[tris[0][0]]));
                for (size_t c = 0; c < ring.size(); c++) p.normals.push_back(n);
                for (auto& tri : tris) p.tris.push_back(tri);
                scenery_tris += ring.size() - 2;
            }
            std::vector<a49::Glb::Prim> list;
            for (auto& [m, p] : prims) if (!p.tris.empty()) list.push_back(std::move(p));
            if (list.empty()) continue;
            if (!panels.empty()) {
                g.textured_mesh("FINISH_BANNER", list);
                banner_built = true;
                report.push_back("finish banner: SCN2 chunk " + std::to_string(k) + " as FINISH_BANNER, its " +
                                 std::to_string(panels.size()) + " CHECKPOINT panels turning to chunk " +
                                 std::to_string(finish_banner) + "'s FINISH on the last lap");
                chunk_count++;
                continue;
            }
            snprintf(line, sizeof(line), "%s_%02zu", scenery ? "PS1_SCENERY" : "PS1_ROAD", k);
            g.textured_mesh(line, list);
            chunk_count++;
        }
    }

    // Collision (docs/sfrush_ps1.md#collision).
    std::vector<std::vector<uint8_t>> coll;
    std::vector<uint8_t> flags;
    if (!disc.chunks("COLLDAT" + t + "/COLLMAP", coll) || !disc.read("COLLDAT" + t + "/FLAGS.DAT", flags)) {
        log += "the collision map couldn't be read\n";
        return false;
    }
    std::vector<Ground> grounds;
    std::vector<Wall> walls;
    std::vector<int> chunk_of_ground;
    size_t fo = 0, b_records = 0;
    for (size_t k = 0; k < coll.size(); k++) {
        const auto& c = coll[k];
        if (c.size() < 0x28) return false;
        uint32_t nv = uint32_t(s32(&c[0x10]));
        int npol = c[0x14], na = c[0x24], nb = c[0x25];
        if (fo + size_t(npol) > flags.size() || 0x28 + size_t(nv) * 12 > c.size()) return false;
        std::vector<V3> V;
        for (uint32_t i = 0; i < nv; i++) {
            const uint8_t* p = &c[0x28 + 12 * i];
            V.push_back(game_coll(s32(p), s32(p + 4), s32(p + 8)));
        }
        size_t o = 0x28 + size_t(nv) * 12;
        auto need = [&](size_t n) { return o + n <= c.size(); };
        for (int i = 0; i < na; i++, o += 0x18) {
            if (!need(0x18)) return false;
            const uint8_t* r = &c[o];
            double d = s32(r), nx = s16(r + 4), nz = s16(r + 6);
            // The segment's bounding box (x min, x max, y min, y max); the segment is the diagonal on the wall's plane
            // (n . p = d, n x 16384).
            double x0 = s32(r + 8), x1 = s32(r + 12), z0 = s32(r + 16), z1 = s32(r + 20);
            auto off = [&](double x, double z) { return std::fabs(nx * x + nz * z - d); };
            if (off(x0, z1) + off(x1, z0) < off(x0, z0) + off(x1, z1)) std::swap(z0, z1);
            // PS1 (x, y) -> game (z, x).
            walls.push_back({ { z0 / 10.0, x0 / 10.0 }, { z1 / 10.0, x1 / 10.0 }, { nz / 16384.0, nx / 16384.0 }, int(k) });
        }
        // Raised walls (B, 0x20): an A wall's plane and box, then two heights (0.1 ft, y down): func_8005AD28 tests
        // a car against it only while its center is over the first (car height - 8 < +0x18), up to the second.
        for (int i = 0; i < nb; i++, o += 0x20) {
            if (!need(0x20)) return false;
            const uint8_t* r = &c[o];
            double d = s32(r), nx = s16(r + 4), nz = s16(r + 6);
            double x0 = s32(r + 8), x1 = s32(r + 12), z0 = s32(r + 16), z1 = s32(r + 20);
            auto off = [&](double x, double z) { return std::fabs(nx * x + nz * z - d); };
            if (off(x0, z1) + off(x1, z0) < off(x0, z0) + off(x1, z1)) std::swap(z0, z1);
            walls.push_back({ { z0 / 10.0, x0 / 10.0 }, { z1 / 10.0, x1 / 10.0 }, { nz / 16384.0, nx / 16384.0 }, int(k),
                              -s32(r + 24) / 10.0, -s32(r + 28) / 10.0 });
            b_records++;
        }
        for (int i = na + nb; i < npol; i++) {
            uint8_t f = flags[fo + size_t(i)];
            size_t size = (f & 7) ? 0x28 : 0x20;
            if (!need(size)) return false;
            const uint8_t* r = &c[o];
            const uint8_t* idx = r + (size == 0x28 ? 0x18 : 0x14);
            Ground gr;
            gr.flags = f;
            std::vector<int> ring;
            for (int q = 0; q < 4; q++) {
                if (idx[q] >= V.size()) return false;
                if (ring.empty() || idx[q] != idx[ring.back()]) ring.push_back(q);
            }
            if (ring.size() == 4 && idx[ring[3]] == idx[ring[0]]) ring.pop_back();
            o += size;
            if (ring.size() < 3) continue;
            gr.n = int(ring.size());
            for (int q = 0; q < gr.n; q++) gr.p[q] = V[idx[ring[q]]];
            gr.tri_count = gr.n - 2;
            gr.tris[0] = { 0, 1, 2 };
            gr.tris[1] = { 0, 2, 3 };
            if (gr.n == 4 && size == 0x28) {
                // A bent quad: its two planes' normals (y down, 16384 = 1) pick the diagonal.
                V3 n0 = game(s16(r + 0x1C) / 16384.0, s16(r + 0x20) / 16384.0, -s16(r + 0x1E) / 16384.0);
                auto tn = [&](int a, int b, int c2) { return a49::unit(a49::cross(gr.p[b] - gr.p[a], gr.p[c2] - gr.p[a])); };
                double split02 = std::max(std::fabs(a49::dot(tn(0, 1, 2), n0)), std::fabs(a49::dot(tn(0, 2, 3), n0)));
                double split13 = std::max(std::fabs(a49::dot(tn(0, 1, 3), n0)), std::fabs(a49::dot(tn(1, 2, 3), n0)));
                if (split13 > split02) {
                    gr.tris[0] = { 0, 1, 3 };
                    gr.tris[1] = { 1, 2, 3 };
                }
            }
            grounds.push_back(gr);
            chunk_of_ground.push_back(int(k));
        }
        if (o != c.size()) {
            snprintf(line, sizeof(line), "collision chunk %zu: %zu of %zu bytes read\n", k, o, c.size());
            log += line;
            return false;
        }
        fo += size_t(npol);
    }
    if (b_records) report.push_back(std::to_string(b_records) + " raised walls (B records), each over its own height band");
    // Ground by surface (FLAGS & 0x18: road, dirt, grass, water). The grass class (the park's grass and sand shoulders)
    // is the custom surface VERGE (track.toml below): COL_GRASS stores wheel material 2, which is Rush 2's water and
    // throws its splash (docs/calspeed_research.md#wheel-effects), so VERGE drives with grass's drag and takes dirt's
    // dust and sound.
    struct Mesh {
        std::vector<V3> verts, normals;
        std::vector<std::array<uint32_t, 3>> tris;
        std::vector<double> segs;    // per vertex: the spline road segment (_SEGMENT), -1 flat; empty for walls
    };
    std::map<std::string, Mesh> by_surface;
    for (const Ground& gr : grounds) {
        int s = gr.flags & 0x18;
        Mesh& m = by_surface[s == 0 ? "ROAD" : s == 0x08 ? "DIRT" : s == 0x10 ? "VERGE" : "WATER"];
        for (int k = 0; k < gr.tri_count; k++) {
            std::array<V3, 3> p{ gr.p[gr.tris[k][0]], gr.p[gr.tris[k][1]], gr.p[gr.tris[k][2]] };
            V3 n = a49::cross(p[1] - p[0], p[2] - p[0]);
            if (a49::norm(n) < 1e-9) continue;
            n = a49::unit(n);
            if (n[1] < 0) {
                std::swap(p[1], p[2]);
                n = n * -1.0;
            }
            uint32_t base = uint32_t(m.verts.size());
            for (int c : { 0, 1, 2 }) {
                m.verts.push_back(gl(p[c]));
                m.normals.push_back(n);
            }
            m.tris.push_back({ base, base + 1, base + 2 });
        }
    }
    // Walls: the PS1's have no height; each becomes a face from 10 ft under the ground beside it to 40 ft over it, cut
    // short under any other ground above (a street over a tunnel).
    size_t steps = 0;
    {
        Mesh& m = by_surface["WALL"];
        for (const Wall& w : walls) {
            V2 d{ w.b[0] - w.a[0], w.b[1] - w.a[1] };
            double len = std::hypot(d[0], d[1]);
            if (len < 0.5) continue;
            if (w.high > w.low) {
                // A raised wall: its own band, from 3 ft under its base (where a car's center, 2-3 ft over the deck,
                // starts to meet it) to its top.
                V3 q[4] = { { w.a[0], w.low - 3.0, w.a[1] }, { w.b[0], w.low - 3.0, w.b[1] }, { w.b[0], w.high, w.b[1] },
                            { w.a[0], w.high, w.a[1] } };
                V3 n{ w.normal[0], 0.0, w.normal[1] };
                if (a49::dot(a49::cross(q[1] - q[0], q[2] - q[0]), n) < 0) std::swap(q[1], q[3]);
                uint32_t base = uint32_t(m.verts.size());
                for (int c : { 0, 1, 2, 3 }) {
                    m.verts.push_back(gl(q[c]));
                    m.normals.push_back(n);
                }
                m.tris.push_back({ base, base + 1, base + 2 });
                m.tris.push_back({ base, base + 2, base + 3 });
                continue;
            }
            double lo = 1e9, hi = -1e9;
            auto seg_dist = [&](const V3& p) {
                double tt = std::clamp(((p[0] - w.a[0]) * d[0] + (p[2] - w.a[1]) * d[1]) / (len * len), 0.0, 1.0);
                return std::hypot(p[0] - (w.a[0] + d[0] * tt), p[2] - (w.a[1] + d[1] * tt));
            };
            for (size_t i = 0; i < grounds.size(); i++) {
                if (chunk_of_ground[i] != w.chunk) continue;
                for (int c = 0; c < grounds[i].n; c++) {
                    if (seg_dist(grounds[i].p[c]) > 60.0) continue;
                    lo = std::min(lo, grounds[i].p[c][1]);
                    hi = std::max(hi, grounds[i].p[c][1]);
                }
            }
            if (lo > hi) continue;
            double bottom = lo - 10.0, top = hi + 40.0;
            for (size_t i = 0; i < grounds.size(); i++) {
                for (int c = 0; c < grounds[i].n; c++) {
                    double y = grounds[i].p[c][1];
                    if (y > hi + 8.0 && y < top + 3.0 && seg_dist(grounds[i].p[c]) < 30.0) top = std::max(hi + 5.0, y - 3.0);
                }
            }
            // Ground passing over the wall's line whose corners are all far from it: Market's street at 61.7 ft, one
            // 490 x 103 ft polygon over a 30 ft wide lower level (29-44 ft), whose side walls otherwise rose through
            // the street as an invisible wall across the road (user report 2026-11-10; docs/sfrush_ps1.md#collision).
            // Sampled every 4 ft along the line, ground counts only where one polygon lies over both sides of it, 8 ft
            // out (a road whose edge is on or just past the line keeps the wall that stops cars driving off it:
            // Embarcadero's ramp at x -2674 overhangs its wall by 2-5 ft), and only that stretch of the wall is cut.
            int samples = std::max(1, int(len / 4.0));
            std::vector<double> tops(size_t(samples) + 1, top);
            {
                double nx = -d[1] / len, nz = d[0] / len;
                double wx0 = std::min(w.a[0], w.b[0]), wx1 = std::max(w.a[0], w.b[0]);
                double wz0 = std::min(w.a[1], w.b[1]), wz1 = std::max(w.a[1], w.b[1]);
                for (const Ground& gr : grounds) {
                    double gx0 = 1e18, gx1 = -1e18, gz0 = 1e18, gz1 = -1e18, gy1 = -1e18;
                    for (int c = 0; c < gr.n; c++) {
                        gx0 = std::min(gx0, gr.p[c][0]);
                        gx1 = std::max(gx1, gr.p[c][0]);
                        gz0 = std::min(gz0, gr.p[c][2]);
                        gz1 = std::max(gz1, gr.p[c][2]);
                        gy1 = std::max(gy1, gr.p[c][1]);
                    }
                    if (gx1 < wx0 - 8.0 || gx0 > wx1 + 8.0 || gz1 < wz0 - 8.0 || gz0 > wz1 + 8.0 || gy1 <= hi + 8.0) continue;
                    // The polygon's height at (x, z), or NaN where it doesn't lie over it.
                    auto height = [&](double x, double z) {
                        for (int k = 0; k < gr.tri_count; k++) {
                            const V3& p0 = gr.p[gr.tris[k][0]];
                            const V3& p1 = gr.p[gr.tris[k][1]];
                            const V3& p2 = gr.p[gr.tris[k][2]];
                            V3 on = a49::cross(p1 - p0, p2 - p0);
                            if (std::fabs(on[1]) < 1e-9) continue;
                            auto side = [&](const V3& a, const V3& b) { return (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]); };
                            double s0 = side(p0, p1), s1 = side(p1, p2), s2 = side(p2, p0);
                            if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
                            return p0[1] - (on[0] * (x - p0[0]) + on[2] * (z - p0[2])) / on[1];
                        }
                        return std::numeric_limits<double>::quiet_NaN();
                    };
                    for (int s = 0; s <= samples; s++) {
                        double t = double(s) / samples, x = w.a[0] + d[0] * t, z = w.a[1] + d[1] * t;
                        double y0 = height(x + nx * 8.0, z + nz * 8.0), y1 = height(x - nx * 8.0, z - nz * 8.0);
                        if (std::isnan(y0) || std::isnan(y1)) continue;
                        double y = std::min(y0, y1);
                        if (y > hi + 8.0 && y < tops[size_t(s)] + 3.0) tops[size_t(s)] = std::max(hi + 5.0, y - 3.0);
                    }
                }
            }
            // One face per run of samples with the same top, each sample's piece reaching halfway to its neighbors.
            V3 n{ w.normal[0], 0.0, w.normal[1] };
            for (int s0 = 0; s0 <= samples;) {
                int s1 = s0;
                while (s1 < samples && tops[size_t(s1) + 1] == tops[size_t(s0)]) s1++;
                double t0 = s0 == 0 ? 0.0 : (s0 - 0.5) / samples, t1 = s1 == samples ? 1.0 : (s1 + 0.5) / samples;
                double h = tops[size_t(s0)];
                V2 pa{ w.a[0] + d[0] * t0, w.a[1] + d[1] * t0 }, pb{ w.a[0] + d[0] * t1, w.a[1] + d[1] * t1 };
                std::array<V3, 4> q{ V3{ pa[0], bottom, pa[1] }, V3{ pb[0], bottom, pb[1] }, V3{ pb[0], h, pb[1] },
                                     V3{ pa[0], h, pa[1] } };
                if (a49::dot(a49::cross(q[1] - q[0], q[2] - q[0]), n) < 0) std::swap(q[1], q[3]);
                uint32_t base = uint32_t(m.verts.size());
                for (int c : { 0, 1, 2, 3 }) {
                    m.verts.push_back(gl(q[c]));
                    m.normals.push_back(n);
                }
                m.tris.push_back({ base, base + 1, base + 2 });
                m.tris.push_back({ base, base + 2, base + 3 });
                s0 = s1 + 1;
            }
        }
        // Steps: a ground polygon too steep to drive between two flat levels (track 4's dead-end lane beside the street
        // climbing to the big loop, glb x 744-762 z -120 m: one road polygon rising 65 ft in 40 ft, 59 degrees, from the
        // lane to the street above, drawn as a rock wall with a chevron sign) stops a PS1
        // car, but Rush 2 drives up any ground face, so the car went "through" the rock wall drawn there. Each gets a
        // wall along its low edge facing downhill, from 3 ft under it to 3 ft over its top; the polygon stays ground so
        // a car coming down from above (behind the one-sided wall) still rolls down it. Flat ground (normal y > 0.9)
        // must lie just past both edges: steep bands of a banked curve (Golden Gate's, 54 degrees, next to other
        // bands or nothing) stay drivable (docs/sfrush_ps1.md#collision).
        auto flat_at = [&](double x, double z, double y) {
            for (const Ground& o : grounds) {
                for (int k = 0; k < o.tri_count; k++) {
                    const V3& p0 = o.p[o.tris[k][0]];
                    const V3& p1 = o.p[o.tris[k][1]];
                    const V3& p2 = o.p[o.tris[k][2]];
                    auto side = [&](const V3& a, const V3& b) { return (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]); };
                    double s0 = side(p0, p1), s1 = side(p1, p2), s2 = side(p2, p0);
                    if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
                    V3 on = a49::cross(p1 - p0, p2 - p0);
                    if (a49::norm(on) < 1e-9) continue;
                    on = a49::unit(on);
                    if (std::fabs(on[1]) < 0.9) continue;
                    double h = p0[1] - (on[0] * (x - p0[0]) + on[2] * (z - p0[2])) / on[1];
                    if (std::fabs(h - y) < 2.0) return true;
                }
            }
            return false;
        };
        for (const Ground& gr : grounds) {
            V3 n = a49::cross(gr.p[1] - gr.p[0], gr.p[2] - gr.p[0]);
            if (a49::norm(n) < 1e-9) continue;
            n = a49::unit(n);
            if (n[1] < 0) n = n * -1.0;
            if (n[1] >= 0.6 || n[1] < 0.2) continue;
            double lo = 1e9, hi = -1e9;
            for (int c = 0; c < gr.n; c++) {
                lo = std::min(lo, gr.p[c][1]);
                hi = std::max(hi, gr.p[c][1]);
            }
            if (hi - lo < 4.0) continue;
            int low[2], nl = 0;
            V3 top{ 0, 0, 0 };
            int nt = 0;
            for (int c = 0; c < gr.n; c++) {
                if (gr.p[c][1] < lo + 1.0 && nl < 2) low[nl++] = c;
                if (gr.p[c][1] > hi - 1.0) {
                    top = top + gr.p[c];
                    nt++;
                }
            }
            if (nl < 2 || nt == 0) continue;
            top = top * (1.0 / nt);
            V3 a = gr.p[low[0]], b = gr.p[low[1]];
            V3 wn{ n[0], 0.0, n[2] };
            wn = a49::unit(wn);
            V3 foot = (a + b) * 0.5;
            if (!flat_at(foot[0] + 2.0 * wn[0], foot[2] + 2.0 * wn[2], lo) || !flat_at(top[0] - 2.0 * wn[0], top[2] - 2.0 * wn[2], hi))
                continue;
            std::array<V3, 4> q{ V3{ a[0], lo - 3.0, a[2] }, V3{ b[0], lo - 3.0, b[2] }, V3{ b[0], hi + 3.0, b[2] },
                                 V3{ a[0], hi + 3.0, a[2] } };
            if (a49::dot(a49::cross(q[1] - q[0], q[2] - q[0]), wn) < 0) std::swap(q[1], q[3]);
            uint32_t base = uint32_t(m.verts.size());
            for (int c : { 0, 1, 2, 3 }) {
                m.verts.push_back(gl(q[c]));
                m.normals.push_back(wn);
            }
            m.tris.push_back({ base, base + 1, base + 2 });
            m.tris.push_back({ base, base + 2, base + 3 });
            steps++;
        }
    }
    if (steps) report.push_back(std::to_string(steps) + " steep ground polygons walled at their foot");
    // (The collision meshes are written after the AI path is read: the spline road is fitted along it.)

    // The AI path: the drones' two lines (docs/sfrush_ps1.md#ai-paths), 20-byte records of 0.1 ft x, y, speed,
    // heading and height (y down).
    auto read_path = [&](const std::string& name, std::vector<V3>& pts) {
        std::vector<uint8_t> p;
        if (!disc.read(name, p) || p.size() < 4) return false;
        uint32_t n = p[0];
        if (4 + size_t(n) * 20 > p.size()) return false;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* r = &p[4 + 20 * i];
            pts.push_back(game_coll(s32(r), s32(r + 16), s32(r + 4)));
        }
        return n >= 8;
    };
    std::vector<V3> left, right;
    if (!read_path("COLLDAT2/PATHDAT" + t + ".DAT", left) || !read_path("COLLDAT2/EXTDAT" + t + ".DAT", right) ||
        left.size() != right.size()) {
        log += "the AI path couldn't be read\n";
        return false;
    }
    // A run-in circuit (track 1, docs/sfrush_ps1.md#run-in): the path is open, from the run-in's start (the Golden Gate
    // Bridge, 3,300 ft before the toll plaza) once round the loop to where it rejoins its start (point 174, 60 ft
    // behind point 29). The race setup (func_80051590's track 1 branch, forward) puts the cars in single file from PS1
    // y 0xE9AC (the back car, 0x80051770) 20 ft apart (0xC8 a car, 0x80051840) toward the run-in, 210 ft before the
    // path's first point; the path is carried back up the run-in's line to 100 ft behind the front car, where Rush 2
    // puts the start line when a track has no START (docs/custom_tracks.md#run-in-circuits).
    if (run_in) {
        const uint32_t ori = exe.in(tr.run_in_grid, 4) ? exe.w(tr.run_in_grid) : 0u;
        if (ori >> 16 != 0x3402) {
            log += "the run-in's grid couldn't be read\n";
            return false;
        }
        constexpr int cars = 8, car_step = 0xC8;   // 0.1 ft
        const double start_y = ((ori & 0xFFFF) - car_step * (cars - 1) + 1000) / 10.0;   // game x = PS1 y, ft
        V3 c0 = (left[0] + right[0]) * 0.5, c1 = (left[1] + right[1]) * 0.5;
        V3 back = c0 - c1;
        double len = a49::norm(back);
        if (len > 0 && std::fabs(back[0]) > 1e-6 && (start_y - c0[0]) / back[0] > 0) {
            V3 shift = back * ((start_y - c0[0]) / back[0]);
            // At the grid's height: the setup's car +4 (height, y down, 1/2560 ft) from lui/ori just before the
            // grid's y (0xFFF52B00: 277 ft up, the bridge deck; the path's own heights run 50 ft under it there).
            const uint32_t lui = exe.in(tr.run_in_grid - 0x68, 8) ? exe.w(tr.run_in_grid - 0x68) : 0u;
            const uint32_t lo = exe.in(tr.run_in_grid - 0x64, 4) ? exe.w(tr.run_in_grid - 0x64) : 0u;
            V3 l0 = left[0] + shift, r0 = right[0] + shift;
            if (lui >> 16 == 0x3C05 && lo >> 16 == 0x34A5) {
                const double up = -double(int32_t((lui & 0xFFFF) << 16 | (lo & 0xFFFF))) / 2560.0;
                l0[1] = r0[1] = up;
            }
            left.insert(left.begin(), l0);
            right.insert(right.begin(), r0);
        }
    }
    // Points every 20 ft or less along the PS1's straight chords (a loop's last back to its first).
    std::vector<V3> lp, rp;
    size_t n = left.size();
    for (size_t i = 0; i < n; i++) {
        if (run_in && i + 1 == n) {
            lp.push_back(left[i]);
            rp.push_back(right[i]);
            break;
        }
        size_t j = (i + 1) % n;
        V3 ci = (left[i] + right[i]) * 0.5, cj = (left[j] + right[j]) * 0.5;
        int steps = std::max(1, int(std::ceil(a49::norm(cj - ci) / 20.0)));
        for (int s = 0; s < steps; s++) {
            double f = double(s) / steps;
            lp.push_back(left[i] + (left[j] - left[i]) * f);
            rp.push_back(right[i] + (right[j] - right[i]) * f);
        }
    }
    size_t m = lp.size();
    std::vector<V3> mid(m);
    for (size_t i = 0; i < m; i++) mid[i] = (lp[i] + rp[i]) * 0.5;

    // The spline road (road_splines, above): the floor near the drones' line is cut into footprints of segments
    // fitted to it; the floor's meshes are written again from the result, every other floor polygon as it was.
    size_t collision_tris = 0;
    {
        auto add = [&](Mesh& mm, std::array<V3, 3> p, double seg) {
            V3 nn = a49::cross(p[1] - p[0], p[2] - p[0]);
            if (a49::norm(nn) < 1e-9) return;
            nn = a49::unit(nn);
            if (nn[1] < 0) {
                std::swap(p[1], p[2]);
                nn = nn * -1.0;
            }
            uint32_t base = uint32_t(mm.verts.size());
            for (int c : { 0, 1, 2 }) {
                mm.verts.push_back(gl(p[c]));
                mm.normals.push_back(nn);
                mm.segs.push_back(seg);
            }
            mm.tris.push_back({ base, base + 1, base + 2 });
        };
        const char* const floor_names[3] = { "ROAD", "DIRT", "VERGE" };
        std::vector<GroundTri> floor;
        std::vector<std::pair<std::array<V3, 3>, int>> steep;     // floor too steep to fit (normal y < 0.2), by class
        for (const Ground& gr : grounds) {
            int s = gr.flags & 0x18;
            if (s == 0x18) continue;     // water: as written above
            int cls = s == 0 ? 0 : s == 0x08 ? 1 : 2;
            for (int k = 0; k < gr.tri_count; k++) {
                std::array<V3, 3> p{ gr.p[gr.tris[k][0]], gr.p[gr.tris[k][1]], gr.p[gr.tris[k][2]] };
                V3 nn = a49::cross(p[1] - p[0], p[2] - p[0]);
                if (a49::norm(nn) < 1e-9) continue;
                nn = a49::unit(nn);
                if (nn[1] < 0) nn = nn * -1.0;
                if (nn[1] < 0.2) steep.push_back({ p, cls });
                else floor.push_back({ p, nn, cls });
            }
        }
        // Golden Gate only (spline_road_track: what was measured on the others).
        RoadSplines rs;
        if (spline_road_track(tr.n)) rs = road_splines(floor, mid, lp, rp, run_in);
        else rs.hint.assign(floor.size(), -1);
        std::vector<a49::CollisionPoly> cand;
        for (size_t i = 0; i < floor.size(); i++) {
            if (rs.hint[i] < 0) continue;
            a49::CollisionPoly cp;
            cp.flags = uint32_t(floor[i].cls) | 0x1000;
            for (const V3& q : floor[i].p) cp.pts.push_back(for_a49(q));
            cp.normal = for_a49(floor[i].n);
            cp.seg = rs.hint[i];
            cand.push_back(std::move(cp));
        }
        std::vector<a49::FootprintTri> foot;
        std::vector<a49::SplinePoly> kept;
        std::vector<a49::FlatPoly> flat;
        if (!rs.records.empty()) {
            // A polygon's pieces may go to any strip at the 4 stations either side of its own (stations nearest
            // first): the strips' bands meet edge to edge, so no piece is offered to two.
            a49::SplineCut cut;
            cut.u_band = rs.u_band;
            cut.candidates = [&rs](const a49::CollisionPoly& p) {
                std::vector<int> ks;
                int per = rs.per_chain, j0 = p.seg % per;
                for (int dj : { 0, -1, 1, -2, 2, -3, 3, -4, 4 }) {
                    int j = j0 + dj;
                    if (rs.open ? (j < 0 || j >= rs.stations - 1) : false) continue;
                    if (!rs.open) j = (j % rs.stations + rs.stations) % rs.stations;
                    for (int i = 0; i < strip_count; i++) ks.push_back(i * per + j);
                }
                return ks;
            };
            a49::cut_spline_road(rs.records, cand, foot, kept, flat, report, cut);
        }
        for (const char* nm : floor_names) by_surface.erase(nm);
        for (size_t i = 0; i < floor.size(); i++) {
            if (rs.hint[i] < 0 || rs.records.empty()) add(by_surface[floor_names[floor[i].cls]], floor[i].p, -1.0);
        }
        for (const auto& [p, cls] : steep) add(by_surface[floor_names[cls]], p, -1.0);
        for (const a49::FootprintTri& ft : foot) {
            add(by_surface[floor_names[ft.flags & 3]], { for_a49(ft.pts[0]), for_a49(ft.pts[1]), for_a49(ft.pts[2]) }, ft.seg);
        }
        auto fan = [&](uint32_t flags, const std::vector<V3>& pts, double seg) {
            for (size_t c = 1; c + 1 < pts.size(); c++) {
                add(by_surface[floor_names[flags & 3]], { for_a49(pts[0]), for_a49(pts[c]), for_a49(pts[c + 1]) }, seg);
            }
        };
        for (const a49::SplinePoly& sp : kept) fan(sp.flags, sp.pts, sp.seg);
        for (const a49::FlatPoly& f : flat) fan(f.flags, f.pts, -1.0);
        snprintf(line, sizeof(line), "spline road: %d stations every %.0f ft along the drones' line (%d fitted from the "
                 "ones before), %d strips of %.0f ft, %zu of %zu floor triangles near it", rs.stations, spline_spacing,
                 rs.weak, strip_count, strip_width, cand.size(), floor.size());
        report.push_back(line);

        for (auto& [surface, mm] : by_surface) {
            if (mm.tris.empty()) continue;
            bool segs = mm.segs.size() == mm.verts.size() &&
                        std::any_of(mm.segs.begin(), mm.segs.end(), [](double s) { return s >= 0; });
            g.mesh("COL_" + surface + "_ps1", mm.verts, mm.normals, mm.tris,
                   segs ? std::vector<std::pair<std::string, std::vector<double>>>{ { "_SEGMENT", mm.segs } }
                        : std::vector<std::pair<std::string, std::vector<double>>>{});
            collision_tris += mm.tris.size();
        }
        if (!rs.records.empty()) {
            int splines = g.empty("ROAD_SPLINES");
            nlohmann::json segments = nlohmann::json::array();
            for (const auto& r : rs.records) {
                nlohmann::json row = nlohmann::json::array();
                for (double v : r) row.push_back(std::isfinite(v) ? v : 0.0);
                segments.push_back(row);
            }
            g.js["nodes"][splines]["extras"] = { { "segments", segments } };
        }
    }
    auto tangent = [&](size_t i) {
        V3 d = run_in ? mid[std::min(i + 1, m - 1)] - mid[i > 0 ? i - 1 : 0] : mid[(i + 1) % m] - mid[(i + m - 1) % m];
        double l = std::hypot(d[0], d[2]);
        return l > 0 ? V2{ d[0] / l, d[2] / l } : V2{ 0.0, 1.0 };
    };
    // Which of the two is on the left in the race direction.
    double side = 0;
    for (size_t i = 0; i < m; i++) {
        V2 tg = tangent(i);
        side += (rp[i][0] - lp[i][0]) * -tg[1] + (rp[i][2] - lp[i][2]) * tg[0];
    }
    // (side is measured in the game frame, the PS1's mirror image: its left is the written track's right.)
    bool right_is_left = side < 0;
    auto ribbon = [&](const std::string& rname, const std::vector<V3>& pts, const std::vector<double>& halves) {
        std::vector<V3> verts, nrms;
        std::vector<std::array<uint32_t, 3>> tris;
        for (size_t i = 0; i < m; i++) {
            V2 tg = tangent(i);
            V3 lat{ -tg[1], 0.0, tg[0] };
            verts.push_back(gl(pts[i] - lat * halves[i]));
            verts.push_back(gl(pts[i] + lat * halves[i]));
            nrms.push_back({ 0, 1, 0 });
            nrms.push_back({ 0, 1, 0 });
        }
        for (size_t i = 0; i < (run_in ? m - 1 : m); i++) {
            uint32_t j = uint32_t((i + 1) % m), ii = uint32_t(i);
            // Wound for gl() (not mirrored): the same turn to_gltf gave these corners the other way round.
            tris.push_back({ 2 * ii, 2 * j, 2 * ii + 1 });
            tris.push_back({ 2 * ii + 1, 2 * j, 2 * j + 1 });
        }
        return g.mesh(rname, verts, nrms, tris);
    };
    std::vector<double> halves(m);
    for (size_t i = 0; i < m; i++) halves[i] = std::max(12.0, std::hypot(lp[i][0] - rp[i][0], lp[i][2] - rp[i][2]) / 2.0);
    ribbon("AIPATH_main", mid, halves);
    // Four lanes across the corridor, left to right, as the drones' lerp of the two lines.
    for (int s = 0; s < 4; s++) {
        double f = s / 3.0;
        std::vector<V3> lane(m);
        for (size_t i = 0; i < m; i++) {
            const V3& a = right_is_left ? rp[i] : lp[i];
            const V3& b = right_is_left ? lp[i] : rp[i];
            lane[i] = a + (b - a) * f;
        }
        ribbon("AIPATH_lane" + std::to_string(s + 1), lane, std::vector<double>(m, 3.0));
    }

    // Gates: TRACK<n>.CPF's four planes (s32 nx, ny, nz, d; 16384 = 1, the path's 0.1 ft units: n . (x, height, y) =
    // d). Plane 0 is the finish line (docs/sfrush_ps1.md#checkpoints).
    std::vector<uint8_t> cpf;
    if (!disc.read("COLLDAT" + t + "/TRACK" + t + ".CPF", cpf) || cpf.size() < 64) {
        log += "the checkpoints couldn't be read\n";
        return false;
    }
    auto plane_side = [&](int k, const V3& p) {
        const uint8_t* r = &cpf[16 * k];
        // Game (x, z) = PS1 (y, x); 0.1 ft.
        double v = s32(r) / 16384.0 * p[2] * 10.0 + s32(r + 8) / 16384.0 * p[0] * 10.0 + s32(r + 4) / 16384.0 * -p[1] * 10.0;
        return v - s32(r + 12) / 16384.0;
    };
    // The PS1's test (func_80054630, checkpoint_test): a car at gate k (car +0x300) passes it when n . p < d while its
    // sector (car +0x2FC, func_800554F0: the last of COLLDAT<n>/BOUNDS.DAT's boxes, s32 x min, x max, y min, y max in
    // 0.1 ft, that holds it) is one of gate k's. Track 4's lists: counts 0x800B2B18, sectors 0x800B2B1C, first index
    // 0x800AA3D8 (one sector a gate: 1, 9, 19, 21). The plane only counts inside those sectors, so a gate goes where
    // the path crosses its plane inside one of them.
    std::vector<uint8_t> bounds;
    if (!disc.read("COLLDAT" + t + "/BOUNDS.DAT", bounds) || bounds.size() < 16) {
        log += "the sector bounds couldn't be read\n";
        return false;
    }
    auto sector_of = [&](const V3& p) {
        // Game (x, z) = PS1 (y, x); 0.1 ft.
        double px = p[2] * 10.0, py = p[0] * 10.0;
        int sector = -1;
        for (size_t b = 0; b + 16 <= bounds.size(); b += 16) {
            const uint8_t* r = &bounds[b];
            if (px >= s32(r) && px <= s32(r + 4) && py >= s32(r + 8) && py <= s32(r + 12)) sector = int(b / 16);
        }
        return sector;
    };
    const uint32_t gate_counts = tr.gate_counts, gate_sectors = tr.gate_sectors, gate_first = tr.gate_first;
    if (!exe.in(gate_counts, 4) || !exe.in(gate_first, 4)) return false;
    auto gate_has = [&](int k, int sector) {
        int n = uint8_t(exe.b(gate_counts + uint32_t(k))), first = uint8_t(exe.b(gate_first + uint32_t(k)));
        for (int s = 0; s < n; s++)
            if (exe.in(gate_sectors + uint32_t(first + s), 1) && uint8_t(exe.b(gate_sectors + uint32_t(first + s))) == sector) return true;
        return false;
    };
    // Where the path crosses plane k (from n . p > d to below) inside one of gate k's sectors, from point i to i + 1.
    auto crosses = [&](int k, size_t i) {
        if (run_in && i + 1 >= m) return false;   // no chord back from an open path's end
        const V3& a = mid[i];
        const V3& b = mid[(i + 1) % m];
        double sa = plane_side(k, a), sb = plane_side(k, b);
        if (!(sa > 0 && sb <= 0)) return false;
        return gate_has(k, sector_of(a + (b - a) * (sa / (sa - sb))));
    };
    std::vector<std::pair<std::string, size_t>> gates;
    // The finish: plane 0's crossing in its sector nearest the path's first point (the PS1's grid stands just past it).
    size_t finish = m;
    for (size_t i = 0; i < m; i++) {
        size_t j = (i + 1) % m;
        if (crosses(0, i) && (finish == m || std::min(j, m - j) < std::min(finish, m - finish))) finish = j;
    }
    if (finish == m) {
        log += "the finish line doesn't cross the AI path\n";
        return false;
    }
    gates.push_back({ "FINISH", finish });
    std::vector<int> gate_plane = { 0 };
    size_t at = finish;
    for (int k = 1; k < 4; k++) {
        size_t found = m;
        for (size_t s = 1; s < m && found == m; s++) {
            size_t i = (at + s) % m;
            if (i == finish) break;
            if (crosses(k, i)) found = (i + 1) % m;
        }
        if (found == m) {
            report.push_back("checkpoint " + std::to_string(k) + ": its plane isn't crossed before the finish, left out");
            continue;
        }
        gates.push_back({ "CHECKPOINT_" + std::to_string(gates.size()), found });
        gate_plane.push_back(k);
        at = found;
    }
    // A run-in circuit: the PS1's finish plane is crossed only on the run-in (the toll plaza; the loop never comes
    // back through it, so a PS1 car passes it once), so it is a checkpoint there, as on the N64's Golden Gate, whose
    // course this is: its lap runs START (the bridge), the toll plaza, two checkpoints round the loop and FINISH at the
    // PS1's gate 3 (docs/sfrush_arcade.md: the arcade version's gates, from the N64's, stand at the PS1's gates). The
    // loop start (where the path's end comes back beside its run-in, a checkpoint as a run-in circuit needs:
    // docs/custom_tracks.md#run-in-circuits) is the path point nearest the end, short of the end's own stretch.
    if (run_in && gates.size() == 4) {
        size_t loop_start = m;
        double best = 1e30;
        for (size_t i = finish; i < m / 2; i++) {
            double d = a49::norm(mid[i] - mid[m - 1]);
            if (d < best) best = d, loop_start = i;
        }
        if (loop_start == m || loop_start <= finish || loop_start >= gates[1].second || best > 300.0) {
            log += "the run-in's loop start couldn't be placed\n";
            return false;
        }
        const size_t toll = finish, g1 = gates[1].second, g2 = gates[2].second, g3 = gates[3].second;
        gates = { { "CHECKPOINT_1", toll }, { "CHECKPOINT_2", loop_start }, { "CHECKPOINT_3", g1 }, { "CHECKPOINT_4", g2 },
                  { "FINISH", g3 } };
        gate_plane = { 0, -1, 1, 2, 3 };
        finish = g3;
    }
    else if (run_in) {
        log += "the run-in's gates couldn't be placed\n";
        return false;
    }
    {
        std::string at_points = "gates at path points";
        for (const auto& gt : gates) {
            const V3& p = mid[gt.second];
            char pos[64];
            snprintf(pos, sizeof(pos), " (%.0f, %.0f, %.0f)", p[0], p[1], p[2]);
            at_points += " " + std::to_string(gt.second) + pos + " sector " + std::to_string(sector_of(p)) + ";";
        }
        report.push_back(at_points + " of " + std::to_string(m));
    }
    // Each gate as wide as the PS1's: its plane across its sector's box (the whole road, 120-178 ft on track 4; the
    // corridor between the AI lines is narrower, and a car outside it would miss the gate), at the path's height.
    for (size_t gi = 0; gi < gates.size(); gi++) {
        const auto& [gname, i] = gates[gi];
        int k = gate_plane[gi];
        if (k < 0) {
            // The loop start: across the path, wide enough for the loop's end and the run-in beside it.
            V2 tg = tangent(i);
            g.empty(gname, a49::gate_matrix(for_a49(mid[i]), for_a49({ tg[0], 0.0, tg[1] }), std::max(halves[i], 30.0) + 60.0));
            continue;
        }
        const uint8_t* r = &cpf[16 * k];
        // In PS1 (x, y) (0.1 ft): n . (x, y) = d, the height term left out (the planes are upright on track 4).
        double nx = s32(r), ny = s32(r + 8), dd = s32(r + 12);
        int sector = sector_of(mid[i]);
        V3 center = mid[i];
        double half = std::max(halves[i], 30.0) + 10.0;
        if (sector >= 0) {
            const uint8_t* bx = &bounds[size_t(sector) * 16];
            double x0 = s32(bx), x1 = s32(bx + 4), y0 = s32(bx + 8), y1 = s32(bx + 12);
            std::vector<V2> ends;
            if (ny != 0)
                for (double x : { x0, x1 }) {
                    double y = (dd - nx * x) / ny;
                    if (y >= y0 && y <= y1) ends.push_back({ x, y });
                }
            if (nx != 0)
                for (double y : { y0, y1 }) {
                    double x = (dd - ny * y) / nx;
                    if (x >= x0 && x <= x1) ends.push_back({ x, y });
                }
            if (ends.size() >= 2) {
                // Game (x, z) = PS1 (y, x) / 10.
                center[0] = (ends[0][1] + ends[1][1]) / 20.0;
                center[2] = (ends[0][0] + ends[1][0]) / 20.0;
                half = std::hypot(ends[0][0] - ends[1][0], ends[0][1] - ends[1][1]) / 20.0;
            }
        }
        // Forward: against the normal (the gate is passed going from n . p > d to below).
        double len = std::hypot(nx, ny);
        g.empty(gname, a49::gate_matrix(for_a49(center), for_a49({ -ny / len, 0.0, -nx / len }), half));
    }

    // The knock-over props (docs/sfrush_ps1.md#props): track 4's table 0x800A58B4 (func_800155F8; 16 bytes: x, y,
    // height in 0.1 ft, kind; -1 ends) places T4KNOCK's cones (kind 0) and trams (kind 1), the models loaded by
    // func_80036C40 at a1 0x380 and 0x280 (coordinates x a1 / 640 ft) and textured from the track's pages (face texture:
    // the texture table entry; flags >> 24: the CLUT). func_80041244 knocks a cone away when a car hits it and runs each
    // tram back and forth between x 6397 and 22115 at 24 a frame (state 1 up, 2 down, turning at either end), wrecking
    // a car within 23.2 ft along and 9.6 ft across it: PROP_KNOCK_ cones, and MOVE_ trams (pingpong) with a wall box.
    // Track 2's table 0x800A55B4 places T2KNOCK's three models (kinds 0-2), every one knocked away as a cone is. The
    // loader's model_prepare (0x8003E714) bakes a turn and an offset into each: 0x800B31EC = 0x400 (90 degrees about
    // the vertical) for every model but the tram, 0x800B31DC = 0x800 (half a turn more about x: upside down) for T2K003,
    // and 0x800B3264 = -0x40 (6.4 ft up, GTE y down in 0.1 ft) for T2K002 and T2K003.
    std::string mover_toml;
    size_t cone_count = 0, tram_count = 0;
    if (tr.prop_table != 0) {
        const uint32_t prop_table = tr.prop_table;
        constexpr int tram_low = 6397, tram_high = 22115, tram_step = 24;   // 0x18FD, 0x5663, a frame
        struct Model {
            const char* file;
            int scale;
            const char* name;    // PROP_KNOCK_<name>_nn; "tram": track 4's mover
            bool spin;           // 90 degrees about the vertical
            bool flip;           // half a turn about x
            double lift;         // ft up
            std::vector<V3> V;
            std::vector<ps1::PsvFace> F;
        };
        std::vector<Model> models;
        if (tr.n == 2) {
            models = { { "T2KNOCK/T2K001.PSV", 0x380, "cone", true, false, 0.0, {}, {} },
                       { "T2KNOCK/T2K002.PSV", 0x280, "traffic_light", true, false, 6.4, {}, {} },
                       { "T2KNOCK/T2K003.PSV", 0x280, "tree", true, true, 6.4, {}, {} } };
        }
        else {
            models = { { "T4KNOCK/CONE.PSV", 0x380, "cone", true, false, 0.0, {}, {} },
                       { "T4KNOCK/TRAM.PSV", 0x280, "tram", false, false, 0.0, {}, {} } };
        }
        for (Model& md : models) {
            std::vector<uint8_t> d;
            if (!disc.read(md.file, d) || !ps1::read_psv(d, md.V, md.F) || md.V.empty()) {
                log += std::string(md.file) + " couldn't be read\n";
                return false;
            }
            double k = md.scale / 640.0;
            for (V3& v : md.V) {
                V3 q{ v[0], v[1], v[2] };
                if (md.flip) q = V3{ q[0], -q[1], -q[2] };
                if (md.spin) q = V3{ -q[1], q[0], q[2] };
                v = V3{ q[0], q[1], q[2] + md.lift / k };
            }
        }
        // A model in its own glTF frame, its origin at its foot (its lowest point): the track's axes (a prop isn't
        // turned), so a vertex (x, y, up) maps as a track point does.
        auto prims_of = [&](const Model& md, double& foot, V3& lo, V3& hi) {
            double k = md.scale / 640.0;
            foot = 1e9;
            for (const V3& v : md.V) foot = std::min(foot, v[2] * k);
            lo = { 1e9, 1e9, 1e9 }, hi = { -1e9, -1e9, -1e9 };
            std::map<int, a49::Glb::Prim> pm;
            for (const ps1::PsvFace& f : md.F) {
                int ci = int(f.flags >> 24);
                if (f.tex >= uint32_t(tex_entries) || ci >= clut_id_count) continue;
                int tx = entry(f.tex, 0), ty = entry(f.tex, 1), voff = entry(f.tex, 3);
                int mat = material(tx & ~63, ty & 256, clut_id(ci));
                a49::Glb::Prim& p = pm[mat];
                p.material = mat;
                // The renderer stores u and v + the entry's v offset as bytes (func_80044E34), as the track's.
                auto texel = [&](size_t c) {
                    return std::array<double, 2>{ double(int(f.uv[c][0]) & 255), double((int(f.uv[c][1]) + voff) & 255) };
                };
                std::array<double, 2> centre{};
                for (size_t c = 0; c < f.uv.size(); c++) centre[0] += texel(c)[0] / f.uv.size(), centre[1] += texel(c)[1] / f.uv.size();
                for (size_t a = 1; a + 1 < f.idx.size(); a++) {
                    // In the PSV's order, as the cars' (ps1_cars.cpp convert): the PSV renderer (func_80044E34)
                    // pairs each vertex with its own coordinate, and gl() keeps the track's handedness.
                    for (size_t c : { size_t(0), a, a + 1 }) {
                        const V3& v = md.V[f.idx[c]];
                        V3 q = gl(game(v[0] * k, v[1] * k, v[2] * k - foot));
                        for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], q[i]), hi[i] = std::max(hi[i], q[i]);
                        p.verts.push_back(q);
                        std::array<double, 2> uv = ps1::texel_inset(texel(c), centre);
                        p.uvs.push_back({ uv[0] / 256.0, uv[1] / 256.0 });
                    }
                }
            }
            std::vector<a49::Glb::Prim> list;
            for (auto& [m2, p] : pm) {
                for (uint32_t i = 0; i < p.verts.size(); i += 3) p.tris.push_back({ i, i + 1, i + 2 });
                p.normals = a49::smooth_normals(p.verts);
                list.push_back(std::move(p));
            }
            return list;
        };
        if (!exe.in(prop_table, 16)) return false;
        for (uint32_t a = prop_table; exe.in(a, 16) && int32_t(exe.w(a)) != -1; a += 16) {
            int32_t x = int32_t(exe.w(a)), y = int32_t(exe.w(a + 4)), h = int32_t(exe.w(a + 8)), kind = int32_t(exe.w(a + 12));
            if (kind < 0 || size_t(kind) >= models.size()) continue;
            const Model& md = models[size_t(kind)];
            double foot;
            V3 lo, hi;
            std::vector<a49::Glb::Prim> list = prims_of(md, foot, lo, hi);
            a49::Glb::Trs trs;
            V3 at = game_coll(x, h, y);   // the model's origin
            trs.translation = gl({ at[0], at[1] + foot, at[2] });
            if (std::string(md.name) != "tram") {
                snprintf(line, sizeof(line), "PROP_KNOCK_%s_%02zu", md.name, cone_count++);
                g.textured_mesh(line, list, trs);
                continue;
            }
            // A tram: from its place to the other end and back. One leg's time at 24 a frame.
            std::string name = "tram_" + std::to_string(tram_count++);
            int node = g.textured_mesh("MOVE_" + name, list, trs);
            int other = x - tram_low < tram_high - x ? tram_high : tram_low;
            V3 end = game_coll(other, h, y);
            double seconds = std::abs(other - x) / double(tram_step) / frames_per_second;
            std::vector<V3> path = { trs.translation, gl({ end[0], end[1] + foot, end[2] }) };
            g.animate(name, node, { 0.0, seconds }, nullptr, nullptr, &path);
            // Its body as a wall box that moves with it (the PS1 wrecks a car it reaches; Rush 2's moving walls push).
            std::vector<V3> bv, bn;
            std::vector<std::array<uint32_t, 3>> bt;
            auto quad = [&](V3 p0, V3 p1, V3 p2, V3 p3, V3 n) {
                uint32_t b = uint32_t(bv.size());
                for (const V3& p : { p0, p1, p2, p3 }) bv.push_back(p), bn.push_back(n);
                bt.push_back({ b, b + 1, b + 2 });
                bt.push_back({ b, b + 2, b + 3 });
            };
            quad({ lo[0], lo[1], hi[2] }, { hi[0], lo[1], hi[2] }, { hi[0], hi[1], hi[2] }, { lo[0], hi[1], hi[2] }, { 0, 0, 1 });
            quad({ hi[0], lo[1], lo[2] }, { lo[0], lo[1], lo[2] }, { lo[0], hi[1], lo[2] }, { hi[0], hi[1], lo[2] }, { 0, 0, -1 });
            quad({ hi[0], lo[1], hi[2] }, { hi[0], lo[1], lo[2] }, { hi[0], hi[1], lo[2] }, { hi[0], hi[1], hi[2] }, { 1, 0, 0 });
            quad({ lo[0], lo[1], lo[2] }, { lo[0], lo[1], hi[2] }, { lo[0], hi[1], hi[2] }, { lo[0], hi[1], lo[2] }, { -1, 0, 0 });
            g.set_parent(g.mesh("COL_WALL_" + name, bv, bn, bt), node);
            mover_toml += "\n[movers." + name + "]\nmode = \"pingpong\"\nstart = \"time 0\"\n";
        }
    }

    // The sky (docs/sfrush_ps1.md#sky): the PS1 draws no sky model, only a grey screen gradient (func_8002FCB0). By day
    // and in snow the track takes Rush 2's cloud dome, as the other converted tracks do (track.toml's sky = "clouds").
    // At night the gradient runs from grey 64 at the top of the screen (25.6 degrees over a level horizon: the
    // 320 x 240 screen at projection distance 250, func_8002C208) down to (0, 0, 25) at the horizon line
    // (docs/sfrush_ps1.md#snow-and-night): a SKY dome colored so, by elevation, with those colors past either end.
    if (cond.condition == Condition::night) {
        constexpr double top_elevation = 25.6, radius = 30000.0;   // degrees, ft
        const std::array<double, 3> top{ 64 / 255.0, 64 / 255.0, 64 / 255.0 }, horizon{ 0.0, 0.0, 25 / 255.0 };
        const double rings[] = { -20, -5, 0, 4, 8, 12, 16, 20, 25.6, 40, 60, 80, 90 };
        constexpr int segments = 32;
        constexpr double pi = 3.14159265358979323846;
        a49::Glb::Prim p;
        p.material = g.material("NIGHT_SKY", std::nullopt, "OPAQUE", false, { 33071, 33071 });
        auto add = [&](double elevation, double azimuth) {
            double e = elevation * pi / 180.0, a = azimuth * pi / 180.0;
            p.verts.push_back(gl({ radius * std::cos(e) * std::cos(a), radius * std::sin(e), radius * std::cos(e) * std::sin(a) }));
            double k = std::clamp(elevation / top_elevation, 0.0, 1.0);
            p.colors.push_back({ horizon[0] + (top[0] - horizon[0]) * k, horizon[1] + (top[1] - horizon[1]) * k,
                                 horizon[2] + (top[2] - horizon[2]) * k, 1.0 });
            p.uvs.push_back({ 0.0, 0.0 });
            V3 n = p.verts.back();
            double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            p.normals.push_back({ -n[0] / len, -n[1] / len, -n[2] / len });   // inward: seen from inside
        };
        for (size_t r = 0; r + 1 < std::size(rings); r++) {
            for (int s = 0; s < segments; s++) {
                double a0 = 360.0 * s / segments, a1 = 360.0 * (s + 1) / segments;
                uint32_t b = uint32_t(p.verts.size());
                add(rings[r], a0), add(rings[r], a1), add(rings[r + 1], a0), add(rings[r + 1], a1);
                for (std::array<uint32_t, 3> tri : { std::array<uint32_t, 3>{ b, b + 1, b + 2 }, std::array<uint32_t, 3>{ b + 1, b + 3, b + 2 } }) {
                    // Front faces (counter-clockwise in glTF) toward the dome's middle.
                    const V3 &A = p.verts[tri[0]], &B = p.verts[tri[1]], &C = p.verts[tri[2]];
                    V3 u{ B[0] - A[0], B[1] - A[1], B[2] - A[2] }, v{ C[0] - A[0], C[1] - A[1], C[2] - A[2] };
                    V3 n{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                    if (n[0] * A[0] + n[1] * A[1] + n[2] * A[2] > 0) std::swap(tri[1], tri[2]);
                    p.tris.push_back(tri);
                }
            }
        }
        g.textured_mesh("SKY", { p });
    }

    // The sea (docs/sfrush_ps1.md#sea): the PS1 has none (grey haze under the horizon), so with Fill Scenery Gaps the
    // extended ground carried the city out over the bay and rose to Golden Gate's deck. A SEA mesh (never drawn, read by
    // the extended ground only) lays water where the N64 track select dioramas have it (TRK1O2, TRK_2O2): Golden Gate's
    // strait under the bridge, Marin an island, the Pacific west of the south end and the bay along the Marina;
    // Embarcadero's bay east of the waterfront road and north of the wharf. Each shore runs along the track's ground,
    // 100-300 ft off it (the gap left to the extended ground's slope), the sea on one side of it out to `far`.
    // Coordinates are track.glb's x and z in feet. Water: the PS1's own water texel (track 2's page 576,256, CLUT 28734:
    // snow 16,49,99, night 0,0,16), flat; Golden Gate has no water of its own and takes it too.
    {
        struct Shore {
            int axis;                                  // 0: the sea is toward x = far; 2: toward z = far
            double far;
            std::vector<std::array<double, 2>> pts;    // (x, z)
        };
        double level = 0;
        std::vector<Shore> shores;
        if (tr.n == 1) {
            level = -150;   // under the lowest ground (the Marina, -144 ft)
            shores = {
                { 0, 3050, { { 3050, -1100 }, { 2650, -800 }, { 2650, 1200 }, { 1950, 2350 }, { 1950, 30000 } } },
                { 2, -30000, { { -400, -2700 }, { 500, -2450 }, { 1500, -2300 }, { 2500, -2200 }, { 3050, -2100 } } },
                { 0, 4800, { { 3050, -30000 }, { 3050, 30000 } } },
                { 0, 30000, { { 4800, -30000 }, { 4800, -2700 } } },
                { 0, 30000, { { 4800, -500 }, { 4800, 30000 } } },
                { 0, 30000, { { 7500, -2700 }, { 7500, -500 } } },   // Marin: an island 1,100 ft past the road's end
            };
        }
        else if (tr.n == 2) {
            level = -8;     // the PS1's water by the wharf (-7.4 ft)
            shores = {
                { 2, -30000, { { -30000, -5500 }, { 2300, -5500 }, { 2500, -5600 }, { 2750, -6350 }, { 3300, -6500 }, { 4080, -6550 } } },
                { 0, 30000, { { 4080, -30000 }, { 4080, -4700 }, { 4250, -4000 }, { 4450, -3250 }, { 4750, -2750 }, { 5150, -2250 },
                              { 5500, -1750 }, { 5650, -1250 }, { 5850, -600 }, { 5850, 30000 } } },
            };
        }
        std::array<uint8_t, 3> water{};
        bool has_water = true;
        if (cond.condition == Condition::snow) water = { 16, 49, 99 };
        else if (cond.condition == Condition::night) water = { 0, 0, 16 };
        else has_water = false;   // no day version of tracks 1 and 2 is built
        if (!shores.empty() && has_water) {
            constexpr int tile = 16;
            std::vector<uint8_t> rgba(size_t(tile) * tile * 4);
            for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = water[0], rgba[i + 1] = water[1], rgba[i + 2] = water[2], rgba[i + 3] = 255;
            a49::Glb::Prim p;
            p.material = g.material("SEA_WATER", g.image("SEA_WATER", a49::png(rgba, tile, tile)), "OPAQUE", false, { 10497, 10497 });
            constexpr double m_per_ft = 0.3048;
            auto add = [&](double x, double z, double u, double v) {
                p.verts.push_back({ x * m_per_ft, level * m_per_ft, z * m_per_ft });
                p.normals.push_back({ 0.0, 1.0, 0.0 });
                p.uvs.push_back({ u, v });
                p.colors.push_back({ 1.0, 1.0, 1.0, 1.0 });
            };
            for (const Shore& s : shores) {
                for (size_t k = 0; k + 1 < s.pts.size(); k++) {
                    const auto &a = s.pts[k], &b = s.pts[k + 1];
                    // The segment and its two ends carried out to `far`.
                    std::array<std::array<double, 2>, 4> q = { a, b, b, a };
                    q[2][s.axis == 0 ? 0 : 1] = s.far;
                    q[3][s.axis == 0 ? 0 : 1] = s.far;
                    uint32_t base = uint32_t(p.verts.size());
                    add(q[0][0], q[0][1], 0, 0), add(q[1][0], q[1][1], 1, 0), add(q[2][0], q[2][1], 1, 1), add(q[3][0], q[3][1], 0, 1);
                    for (std::array<uint32_t, 3> tri : { std::array<uint32_t, 3>{ base, base + 1, base + 2 }, std::array<uint32_t, 3>{ base, base + 2, base + 3 } }) {
                        // Facing up: counter-clockwise seen from above in glTF (x right, z toward the viewer).
                        const V3 &A = p.verts[tri[0]], &B = p.verts[tri[1]], &C = p.verts[tri[2]];
                        double ny = (B[2] - A[2]) * (C[0] - A[0]) - (B[0] - A[0]) * (C[2] - A[2]);
                        if (ny < 0) std::swap(tri[1], tri[2]);
                        p.tris.push_back(tri);
                    }
                }
            }
            g.textured_mesh("SEA", { p });
            report.push_back("sea: " + std::to_string(p.tris.size()) + " triangles at " + std::to_string(int(level)) + " ft (Fill Scenery Gaps only)");
        }
    }

    // The PS1's timer (docs/sfrush_ps1.md#race-times): the race setup (0x80015D60) starts the clock at 45 on track 4
    // forward (addiu 0x2D at 0x80015D80); passing gate k (checkpoint_passed, 0x80054130) adds byte k of 0x800B2AE8
    // (the stretch after it: 26, 29, 11, 32), and 5 more (0x800542D8) while the player has passed fewer than 3 gates
    // (the first two checkpoints of lap 1). Rush 2 grants checkpoint k's time when it is passed, too. The times stand
    // as seconds: on the courses both versions have, the PS1's lap times are the N64's (real seconds) to 10%.
    // In snow checkpoint_passed adds 8 more at every gate, the finish line too (0x80054358, while gp+0x40 is 1), and the
    // race setup starts the clock at 60 whatever the track (0x80015E04). The other tracks (forward): 38 on tracks 1 and 2
    // (0x80015DF0, 0x80015DCC), 60 on track 3 (0x80015DA8), each gate's time from their own tables (gp+0xC60, set by
    // checkpoint_passed per track index), and no first-gates bonus.
    // Snow keeps no times of its own: the PS1's 8 s more a gate were for its snow's two halvings, which the recomp keeps
    // one of (the quarter from 64 mph played too slowly), so the race time model times it with the track's traction
    // (src/race_time_model.cpp), the clock as tight as on any converted track.
    std::string race_toml;
    if (cond.condition == Condition::snow) {
        report.push_back("race times: the race time model's, with the snow's traction");
    }
    else {
        constexpr uint32_t snow_insn = 0x80054358;
        const uint32_t start_insn = cond.condition == Condition::snow ? snow_start_insn : tr.start_insn;
        const uint32_t gate_times = tr.gate_times, bonus_insn = tr.bonus_insn;
        if (!exe.in(start_insn, 4) || !exe.in(gate_times, 4) || !exe.in(snow_insn, 4) ||
            exe.w(start_insn) >> 16 != 0x2402 || exe.w(snow_insn) >> 16 != 0x2442 ||
            (bonus_insn != 0 && (!exe.in(bonus_insn, 4) || exe.w(bonus_insn) >> 16 != 0x2482))) {
            log += "the race times couldn't be read\n";
            return false;
        }
        int start = int(exe.w(start_insn) & 0xFFFF), bonus = bonus_insn != 0 ? int(exe.w(bonus_insn) & 0xFFFF) : 0;
        int snow = cond.condition == Condition::snow ? int(exe.w(snow_insn) & 0xFFFF) : 0;
        // A gate's time by its PS1 plane; a run-in's loop start (no PS1 gate) adds nothing.
        auto gate_time = [&](int k) { return k < 0 ? 0 : int(uint8_t(exe.b(gate_times + uint32_t(k)))) + snow; };
        int finish_plane = 0;
        for (size_t gi = 0; gi < gates.size(); gi++) if (gates[gi].first == "FINISH") finish_plane = gate_plane[gi];
        race_toml = "[race]\nstart_time = " + std::to_string(start) + "\nfinish_time = " + std::to_string(gate_time(finish_plane)) +
                    "\ncheckpoint_time = [";
        int listed = 0;
        for (size_t gi = 0; gi < gates.size(); gi++) {
            if (gates[gi].first == "FINISH") continue;
            int k = gate_plane[gi];
            race_toml += (listed > 0 ? ", [" : "[") + std::to_string(gate_time(k) + (listed < 2 ? bonus : 0)) + ", " +
                         std::to_string(gate_time(k)) + "]";
            listed++;
        }
        race_toml += "]\n\n";
        report.push_back("race times: start " + std::to_string(start) + " s, finish +" + std::to_string(gate_time(finish_plane)) +
                         " s, " + race_toml.substr(race_toml.find("checkpoint_time")));
    }

    // What the conditions change in track.toml (docs/sfrush_ps1.md#snow-and-night).
    std::string condition_toml, look_toml, menu_toml = std::string("variant = \"") + cond.variant + "\"\n";
    // Track 4's versions are of its day track; the others' of the N64 course (without the N64 ROM they are tracks of
    // their own after the SF Rush tracks: `after`).
    if (cond.condition != Condition::day) menu_toml += "variant_of = \"" + std::string(tr.course ? tr.course : tr.id) + "\"\n";
    if (cond.condition == Condition::night) {
        // The fog: clut_upload_shaded (0x80014F38) steps each channel of a texture's CLUT down to 1 of 31 at night
        // (up to 26 by day), so distant scenery fades to black (8, 8, 8). The sky is the dome above.
        // The cars: the PS1 draws them with their night paint pages and textures (CARSGFXN, func_80030930), the paint
        // ramps (CLUT entries 1-6) at about a quarter of the day's; here every car, whatever game it is from, is tinted
        // by that ratio, measured over every car texture both sets have (car_light). The PS1's light glow sprites
        // (light_sprite_draw, docs/sfrush_ps1.md#snow-and-night) aren't ported.
        std::array<double, 3> day{}, night{};
        for (const char* f : { "BLACK", "BLUE", "BROWN", "GREEN", "ORANGE", "PURPLE", "RED", "WHITE", "YELLOW", "BMW01", "BMW02",
                               "BUGETT01", "BUGETT02", "CAM01", "CAM02", "CONCEP01", "CONCEP02", "HUTCH01", "JEP01", "JET01",
                               "MINI01", "PIGGY14", "SUPRA01", "SUPRA02", "VIPER01", "VIPER02", "VWBUG01", "VWBUG02",
                               "VWBUS01", "VWBUS02", "BBUS", "TATS12" }) {
            std::vector<uint8_t> a, b;
            if (!disc.read(std::string("CARSGFX/") + f + ".GFX", a) || !disc.read(std::string("CARSGFXN/") + f + ".GFX", b) ||
                a.size() < 40 || b.size() < 40) continue;
            for (int i = 1; i <= 6; i++) {
                uint16_t ca = u16(&a[8 + 2 * i]), cb = u16(&b[8 + 2 * i]);
                for (int k = 0; k < 3; k++) day[k] += (ca >> (5 * k)) & 31, night[k] += (cb >> (5 * k)) & 31;
            }
        }
        char light[8];
        auto channel = [&](int k) { return day[k] > 0 ? int(std::lround(std::clamp(night[k] / day[k], 0.0, 1.0) * 255.0)) : 255; };
        snprintf(light, sizeof(light), "#%02x%02x%02x", channel(0), channel(1), channel(2));
        look_toml = std::string("[look]\nfog_color = \"#080808\"\nsky = \"none\"\ncar_light = \"") + light + "\"\n\n";
        report.push_back(std::string("night: car light ") + light);
    }
    else {
        // The PS1's fog: clut_upload_shaded (0x80014F38) steps each channel of a texture's CLUT up by one a band
        // to 26 of 31 (day and snow), so distant scenery fades to 214 grey. The sky is Rush 2's cloud dome, scrolling
        // at 0.2 as on most Rush 2 tracks and the N64 SF Rush ones (src/rush1/track1.cpp).
        look_toml = "[look]\nfog_color = \"#d6d6d6\"\nsky = \"clouds\"\ncloud_scroll = 0.2\n\n";
    }
    if (cond.condition == Condition::snow) {
        // car_integrate (0x8005D190), in snow: every car's horizontal forces for the frame (thrust and the tires'
        // friction) halved from speed 151 (slti 0x97 at 0x8005D204) and again from 301 (0x12D at 0x8005D240). Speed is
        // the horizontal |v| / 32 (v in 1/2560 ft a frame), at the car conversion's frames_per_second. Rush 2 scales
        // the same forces ([handling] traction, src/track_traction.cpp). Only the first step is kept: with the second,
        // speeding up from 64 mph took four times as long as dry, which played far too slowly; halved from 32 mph, it
        // takes twice as long and turning and braking are halved too.
        constexpr uint32_t first_insn = 0x8005D204, second_insn = 0x8005D240;
        if (!exe.in(first_insn, 4) || !exe.in(second_insn, 4) || exe.w(first_insn) >> 16 != 0x2842 || exe.w(second_insn) >> 16 != 0x2842) {
            log += "the snow handling couldn't be read\n";
            return false;
        }
        auto mph = [&](uint32_t insn) { return int(exe.w(insn) & 0xFFFF) * 32.0 / 2560.0 * frames_per_second * 3600.0 / 5280.0; };
        snprintf(line, sizeof(line), "[handling]\ntraction = [[%.1f, 0.5]]\n\n", mph(first_insn));
        condition_toml = line;
        report.push_back(std::string("snow: ") + line);
    }

    const std::string dir = "tracks/" + folder + "/";
    out[dir + "track.glb"] = g.bytes();
    // The track select diorama (docs/sfrush_ps1.md#track-select-diorama): hand-made round the AI path. Without it
    // (a broken layout file) the track select shows the route preview instead. Track 4's only: the versions of the N64
    // courses show their course's diorama (src/rush2049/track2049_menu.cpp, as the SF Rush arcade versions do).
    std::string diorama_toml;
    if (tr.course == nullptr) {
        std::vector<std::array<double, 3>> route(m);
        for (size_t i = 0; i < m; i++) route[i] = gl(mid[i]);
        std::vector<uint8_t> diorama;
        if (ps1::build_diorama(route, assets_dir / (std::string(track_id) + "_diorama.json"), diorama, log)) {
            out[dir + "diorama.glb"] = std::move(diorama);
            diorama_toml = "diorama = \"diorama.glb\"\n";
        }
    }
    const char* described = cond.condition == Condition::snow ? " in the snow" : cond.condition == Condition::night ? " at night" : "";
    out[dir + "track.toml"] = a49::text_bytes(
        // smooth_height: the PS1's road polygons are coarse (a crease of 1-6 degrees every 57 ft on Golden Gate's
        // lanes), and with only the blended tilt each crease still kicked the car on straights (user report 2026-11-09;
        // docs/sfrush_ps1.md#collision).
        "[track]\nname = \"" + track_name + "\"\nsmooth_height = true\n" + (run_in ? "run_in = true\n" : "") +
        "author = \"San Francisco Rush, PlayStation (converted)\"\n"
        "description = \"Track " + t + " of the PlayStation San Francisco Rush" + described + "\"\n\n" + race_toml +
        // The grass class (COL_VERGE, above): grass's drag, dirt's dust and sound.
        "[surfaces.verge]\nlike = \"dirt\"\ndrag = 15\n\n" + ai_speed_toml(tr.n) + condition_toml + look_toml +
        "[menu]\nlogo = \"logo.png\"\npreview = \"preview.png\"\nafter = \"sfrush\"\n" + menu_toml + diorama_toml + mover_toml);
    // The track select picture: the PS1's own (TITLETGA/TRK<n>N.TGA, 512 x 168, 15-bit, rows top-down whatever the
    // header says), a map of the course with its districts named, whole: the route preview keeps its 512 x 168 texels
    // and shows it between the carousel arrows (docs/custom_tracks.md#track-select-art).
    {
        std::vector<uint8_t> tga;
        if (!disc.read("TITLETGA/TRK" + t + "N.TGA", tga) || tga.size() < 18) {
            log += "TITLETGA/TRK" + t + "N.TGA couldn't be read\n";
            return false;
        }
        int w = tga[12] | tga[13] << 8, h = tga[14] | tga[15] << 8;
        size_t at = 18 + size_t(tga[0]);
        if (tga[2] != 2 || w <= 0 || h <= 0 || at + size_t(w) * h * 2 > tga.size()) {
            log += "TITLETGA/TRK" + t + "N.TGA isn't the 15-bit picture expected\n";
            return false;
        }
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint8_t* s = &tga[at + (size_t(y) * w + size_t(x)) * 2];
                uint16_t c = uint16_t(s[0] | s[1] << 8);
                uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
                for (int k = 0; k < 3; k++) {
                    uint8_t v = uint8_t(((c >> (10 - 5 * k)) & 31) << 3);
                    p[k] = uint8_t(v | v >> 5);
                }
                p[3] = 255;
            }
        }
        out[dir + "preview.png"] = a49::png(rgba, w, h);
    }
    {
        std::ifstream f(assets_dir / (std::string(track_id) + "_logo.png"), std::ios::binary);
        if (!f) {
            log += "missing " + rush2::upscale::path_utf8(assets_dir / (std::string(track_id) + "_logo.png")) + "\n";
            return false;
        }
        out[dir + "logo.png"].assign(std::istreambuf_iterator<char>(f), {});
    }
    snprintf(line, sizeof(line), "%zu knock-over props, %zu trams", cone_count, tram_count);
    report.push_back(line);

    snprintf(line, sizeof(line), "wrote %s: %zu chunks, %zu scenery triangles, %zu textures, %zu collision triangles "
             "(%zu walls), %zu gates, AI path of %zu points\n", dir.c_str(), chunk_count, scenery_tris, materials.size(),
             collision_tris, walls.size(), gates.size(), m);
    log += line;
    for (const auto& [gname, i] : gates) {
        snprintf(line, sizeof(line), "  %s at AI path point %zu, corridor half width %.0f ft\n", gname.c_str(), i, halves[i]);
        log += line;
    }
    for (const std::string& r : report) log += r + "\n";
    return true;
}

bool ps1::build_track(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log) {
    for (const TrackInfo& tr : tracks) {
        for (const ConditionInfo& c : conditions) {
            if (c.condition == Condition::day && !tr.day) continue;
            if (!build_condition(disc, assets_dir, out, log, tr, c)) return false;
        }
    }
    return true;
}
