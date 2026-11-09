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
    constexpr const char* track_id = "chinatown";
    constexpr const char* track_name = "Chinatown";
    constexpr int track = 4;

    // RUSH.EXE (docs/sfrush_ps1.md#textures).
    constexpr uint32_t exe_base = ps1::Exe::base;
    constexpr uint32_t load_image = 0x80092C58, load_clut = 0x80014F38;
    constexpr uint32_t page_loader = 0x80039A88, clut_loader = 0x8003C408;   // track 4's
    constexpr uint32_t tex_table = 0x800A77C0;                               // tracks 1, 3 and 4 (0x800A7478: track 2)
    constexpr int tex_entries = 35;
    constexpr uint32_t clut_ids = 0x800A7B08;
    constexpr int clut_id_count = 64;

    int16_t s16(const uint8_t* p) { return int16_t(p[0] | p[1] << 8); }
    uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    int32_t s32(const uint8_t* p) { return int32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

    using Exe = ps1::Exe;

    // The page loader's LoadImage RECTs and the CLUT loader's (x, y), in chunk order: the loaders are straight-line
    // code, so a scan that follows constant registers and the RECT on the stack finds them (vram.py scan).
    struct Calls {
        std::vector<std::array<int, 4>> images;
        std::vector<std::array<int, 2>> cluts;
    };
    void scan(const Exe& e, uint32_t start, Calls& out) {
        std::map<int, uint32_t> regs{ { 0, 0 } };
        std::map<int, uint32_t> stack;
        uint32_t end = e.func_end(start);
        auto apply = [&](uint32_t w) {
            int op = int(w >> 26), rs = int((w >> 21) & 31), rt = int((w >> 16) & 31);
            uint32_t imm = w & 0xFFFF;
            int32_t simm = int16_t(imm);
            if (op == 9 && regs.count(rs)) regs[rt] = regs[rs] + uint32_t(simm);
            else if (op == 0xD && regs.count(rs)) regs[rt] = regs[rs] | imm;
            else if (op == 0xF) regs[rt] = imm << 16;
            else if (op == 0x29 && rs == 29 && regs.count(rt)) stack[simm] = regs[rt] & 0xFFFF;
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
                if (t == load_image) out.images.push_back({ st(0x10), st(0x12), st(0x14), st(0x16) });
                else if (t == load_clut) out.cluts.push_back({ regs.count(4) ? int(regs[4]) : -1, regs.count(5) ? int(regs[5]) : -1 });
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
}

bool ps1::build_track(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log) {
    char line[256];
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
    scan(exe, page_loader, pages);
    scan(exe, clut_loader, cluts);
    std::vector<std::vector<uint8_t>> page_chunks, clut_chunks;
    const std::string t = std::to_string(track);
    if (!disc.chunks("T" + t + "PAGE/T" + t + "PAGE", page_chunks) || !disc.chunks("T" + t + "CLUT/T" + t + "CLUT", clut_chunks) ||
        pages.images.size() != page_chunks.size() || cluts.cluts.size() != clut_chunks.size()) {
        snprintf(line, sizeof(line), "texture pages: %zu loads for %zu chunks, CLUTs: %zu for %zu\n", pages.images.size(),
                 page_chunks.size(), cluts.cluts.size(), clut_chunks.size());
        log += line;
        return false;
    }
    Vram vram;
    for (size_t k = 0; k < page_chunks.size(); k++) {
        auto [x, y, w, h] = pages.images[k];
        const auto& c = page_chunks[k];
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 1024 || y + h > 512 || c.size() < 0x28 + size_t(w) * h * 2) {
            log += "a texture page's RECT is outside VRAM\n";
            return false;
        }
        for (int r = 0; r < h; r++) {
            for (int i = 0; i < w; i++) vram.v[size_t(y + r) * 1024 + size_t(x + i)] = u16(&c[0x28 + 2 * (size_t(r) * w + i)]);
        }
    }
    for (size_t k = 0; k < clut_chunks.size(); k++) {
        auto [x, y] = cluts.cluts[k];
        if (x < 0 || y < 0 || x + 16 > 1024 || y >= 512 || clut_chunks[k].size() < 40) return false;
        for (int i = 0; i < 16; i++) vram.v[size_t(y) * 1024 + size_t(x + i)] = u16(&clut_chunks[k][8 + 2 * i]);
    }
    if (!exe.in(tex_table, tex_entries * 24) || !exe.in(clut_ids, clut_id_count * 4)) return false;
    auto entry = [&](uint32_t i, int f) { return s32(&exe.data[tex_table - exe_base + i * 24 + f * 4]); };
    auto clut_id = [&](int i) { return int(exe.w(clut_ids + uint32_t(i) * 4)); };

    // Scenery: the road and the scenery chunks, one material per (texture page, CLUT).
    a49::Glb g("rush2-recomp src/sfrush_ps1 (PS1 SF Rush track " + t + ")", true);
    std::map<std::array<int, 3>, int> materials;
    auto material = [&](int tx, int ty, int clut) {
        std::array<int, 3> key{ tx, ty, clut };
        auto it = materials.find(key);
        if (it != materials.end()) return it->second;
        std::vector<uint8_t> rgba(256 * 256 * 4);
        bool clear = false;
        for (int v = 0; v < 256; v++) {
            for (int u = 0; u < 256; u++) {
                auto c = vram.texel(tx, ty, clut, u, v);
                std::memcpy(&rgba[(size_t(v) * 256 + u) * 4], c.data(), 4);
                clear = clear || c[3] == 0;
            }
        }
        snprintf(line, sizeof(line), "P%d_%d_C%d", tx, ty, clut);
        int image = g.image(line, a49::png(rgba, 256, 256));
        // Clamped: a page holds several textures, and a face's UVs never leave its own.
        int m = g.material(line, image, clear ? "MASK" : "OPAQUE", false, { 33071, 33071 });
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
    int start_banner = -1, finish_banner = -1;
    {
        constexpr uint32_t id_insn = 0x80044628, alt_insn = 0x80044638, chunk_table = 0x800B5C88;
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
    bool banner_built = false;
    for (const char* kind : { "ROAD2", "SCN2" }) {
        std::vector<std::vector<uint8_t>> raw;
        if (!disc.chunks("T" + t + "ROAD/T" + t + kind, raw)) {
            log += std::string("T") + t + "ROAD/T" + t + kind + ".DAT couldn't be read\n";
            return false;
        }
        const bool scenery = std::string(kind) == "SCN2";
        for (size_t k = 0; k < raw.size(); k++) {
            if (scenery && banner_built && int(k) == finish_banner) continue;   // drawn as the FINISH material
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
            if (scenery && int(k) == start_banner && finish_banner > start_banner && size_t(finish_banner) < raw.size() &&
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
                int m = material(tx & ~63, ty & 256, clut_id(f.clut));
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
            V3 a0{ w.a[0], bottom, w.a[1] }, b0{ w.b[0], bottom, w.b[1] }, a1{ w.a[0], top, w.a[1] }, b1{ w.b[0], top, w.b[1] };
            V3 n{ w.normal[0], 0.0, w.normal[1] };
            std::array<V3, 4> q{ a0, b0, b1, a1 };
            if (a49::dot(a49::cross(q[1] - q[0], q[2] - q[0]), n) < 0) std::swap(q[1], q[3]);
            uint32_t base = uint32_t(m.verts.size());
            for (int c : { 0, 1, 2, 3 }) {
                m.verts.push_back(gl(q[c]));
                m.normals.push_back(n);
            }
            m.tris.push_back({ base, base + 1, base + 2 });
            m.tris.push_back({ base, base + 2, base + 3 });
        }
    }
    size_t collision_tris = 0;
    for (auto& [surface, m] : by_surface) {
        if (m.tris.empty()) continue;
        g.mesh("COL_" + surface + "_ps1", m.verts, m.normals, m.tris);
        collision_tris += m.tris.size();
    }

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
    // Points every 20 ft or less along the PS1's straight chords.
    std::vector<V3> lp, rp;
    size_t n = left.size();
    for (size_t i = 0; i < n; i++) {
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
    auto tangent = [&](size_t i) {
        V3 d = mid[(i + 1) % m] - mid[(i + m - 1) % m];
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
        for (size_t i = 0; i < m; i++) {
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
    constexpr uint32_t gate_counts = 0x800B2B18, gate_sectors = 0x800B2B1C, gate_first = 0x800AA3D8;
    if (!exe.in(gate_counts, 4) || !exe.in(gate_first, 4)) return false;
    auto gate_has = [&](int k, int sector) {
        int n = uint8_t(exe.b(gate_counts + uint32_t(k))), first = uint8_t(exe.b(gate_first + uint32_t(k)));
        for (int s = 0; s < n; s++)
            if (exe.in(gate_sectors + uint32_t(first + s), 1) && uint8_t(exe.b(gate_sectors + uint32_t(first + s))) == sector) return true;
        return false;
    };
    // Where the path crosses plane k (from n . p > d to below) inside one of gate k's sectors, from point i to i + 1.
    auto crosses = [&](int k, size_t i) {
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
    std::string mover_toml;
    size_t cone_count = 0, tram_count = 0;
    {
        constexpr uint32_t prop_table = 0x800A58B4;
        constexpr int tram_low = 6397, tram_high = 22115, tram_step = 24;   // 0x18FD, 0x5663, a frame
        struct Model {
            const char* file;
            int scale;
            std::vector<V3> V;
            std::vector<ps1::PsvFace> F;
        };
        Model models[2] = { { "T4KNOCK/CONE.PSV", 0x380, {}, {} }, { "T4KNOCK/TRAM.PSV", 0x280, {}, {} } };
        for (Model& md : models) {
            std::vector<uint8_t> d;
            if (!disc.read(md.file, d) || !ps1::read_psv(d, md.V, md.F) || md.V.empty()) {
                log += std::string(md.file) + " couldn't be read\n";
                return false;
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
            if (kind != 0 && kind != 1) continue;
            const Model& md = models[kind];
            double foot;
            V3 lo, hi;
            std::vector<a49::Glb::Prim> list = prims_of(md, foot, lo, hi);
            a49::Glb::Trs trs;
            V3 at = game_coll(x, h, y);   // the model's origin
            trs.translation = gl({ at[0], at[1] + foot, at[2] });
            if (kind == 0) {
                snprintf(line, sizeof(line), "PROP_KNOCK_cone_%02zu", cone_count++);
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

    // The sky (docs/sfrush_ps1.md#sky): the PS1 draws no sky model, only a grey screen gradient (func_8002FCB0). The
    // track takes Rush 2's cloud dome, as the other converted tracks do (track.toml's sky = "clouds").

    // The PS1's timer (docs/sfrush_ps1.md#race-times): the race setup (0x80015D60) starts the clock at 45 on track 4
    // forward (addiu 0x2D at 0x80015D80); passing gate k (checkpoint_passed, 0x80054130) adds byte k of 0x800B2AE8
    // (the stretch after it: 26, 29, 11, 32), and 5 more (0x800542D8) while the player has passed fewer than 3 gates
    // (the first two checkpoints of lap 1). Rush 2 grants checkpoint k's time when it is passed, too. The times stand
    // as seconds: on the courses both versions have, the PS1's lap times are the N64's (real seconds) to 10%.
    std::string race_toml;
    {
        constexpr uint32_t start_insn = 0x80015D80, gate_times = 0x800B2AE8, bonus_insn = 0x800542D8;
        if (!exe.in(start_insn, 4) || !exe.in(bonus_insn, 4) || !exe.in(gate_times, 4) ||
            exe.w(start_insn) >> 16 != 0x2402 || exe.w(bonus_insn) >> 16 != 0x2482) {
            log += "the race times couldn't be read\n";
            return false;
        }
        int start = int(exe.w(start_insn) & 0xFFFF), bonus = int(exe.w(bonus_insn) & 0xFFFF);
        auto gate_time = [&](int k) { return int(uint8_t(exe.b(gate_times + uint32_t(k)))); };
        race_toml = "[race]\nstart_time = " + std::to_string(start) + "\nfinish_time = " + std::to_string(gate_time(0)) +
                    "\ncheckpoint_time = [";
        for (size_t gi = 1; gi < gates.size(); gi++) {
            int k = gate_plane[gi];
            race_toml += (gi > 1 ? ", [" : "[") + std::to_string(gate_time(k) + (gi < 3 ? bonus : 0)) + ", " +
                         std::to_string(gate_time(k)) + "]";
        }
        race_toml += "]\n\n";
        report.push_back("race times: start " + std::to_string(start) + " s, finish +" + std::to_string(gate_time(0)) +
                         " s, " + race_toml.substr(race_toml.find("checkpoint_time")));
    }

    const std::string dir = std::string("tracks/") + track_id + "/";
    out[dir + "track.glb"] = g.bytes();
    // The track select diorama (docs/sfrush_ps1.md#track-select-diorama): hand-made round the AI path. Without it
    // (a broken layout file) the track select shows the route preview instead.
    std::string diorama_toml;
    {
        std::vector<std::array<double, 3>> route(m);
        for (size_t i = 0; i < m; i++) route[i] = gl(mid[i]);
        std::vector<uint8_t> diorama;
        if (ps1::build_diorama(route, assets_dir / (std::string(track_id) + "_diorama.json"), diorama, log)) {
            out[dir + "diorama.glb"] = std::move(diorama);
            diorama_toml = "diorama = \"diorama.glb\"\n";
        }
    }
    out[dir + "track.toml"] = a49::text_bytes(
        std::string("[track]\nname = \"") + track_name + "\"\n" +
        "author = \"San Francisco Rush, PlayStation (converted)\"\n"
        "description = \"Track 4 of the PlayStation San Francisco Rush\"\n\n" + race_toml +
        // The grass class (COL_VERGE, above): grass's drag, dirt's dust and sound.
        "[surfaces.verge]\nlike = \"dirt\"\ndrag = 15\n\n" +
        // The PS1's fog: clut_upload_shaded (0x80014F38) steps each channel of a texture's CLUT up by one a band
        // to 26 of 31 (day), so distant scenery fades to 214 grey. The sky is Rush 2's cloud dome, scrolling at 0.2 as
        // on most Rush 2 tracks and the N64 SF Rush ones (src/rush1/track1.cpp).
        "[look]\nfog_color = \"#d6d6d6\"\nsky = \"clouds\"\ncloud_scroll = 0.2\n\n"
        "[menu]\nlogo = \"logo.png\"\npreview = \"preview.png\"\nafter = \"sfrush\"\n" + diorama_toml + mover_toml);
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
    snprintf(line, sizeof(line), "%zu knock-over cones, %zu trams", cone_count, tram_count);
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
