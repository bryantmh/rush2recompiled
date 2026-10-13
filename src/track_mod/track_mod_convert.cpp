// Custom track converter (include/track_mod.h, docs/custom_tracks.md): track.glb + track.toml -> the five files of a
// Rush 2 track slot (geometry, placement, collision, forward and backward AI path) and its per-track table values.
//
// Spaces: glTF is meters, y up, right handed. The game is feet, y up, with x the glTF x negated (the model dump's
// convention, docs/asset_catalog.md); model vertices are 1/16 ft relative to their placement record.
//
// Objects are recognized by name (a Blender ".001" suffix is ignored):
//   COL_<SURFACE>[_anything]  collision only, not drawn. ROAD, DIRT, GRASS, WALL, WATER, KILL or a [surfaces] name.
//   AIPATH_main               the AI path: a curve with Extrude, exported as a ribbon of left/right vertex pairs.
//   FINISH, CHECKPOINT_n      gates: Empty, green (Blender Y = glTF -Z) arrow forward, X scale = half the width (m).
//   START_n                   ignored: Rush 2 lines the grid up behind FINISH itself (said in the report).
//   AISPEED_<mph>             caps the AI's target speed from the nearest path point on (0 lifts the cap).
//   SKY                       sky dome (not supported yet).
//   PROP_<CLASS>              props (not supported yet).
//   anything else with a mesh the visible track.
//
// Formats: geometry docs/rush2049_research/geometry.md section 2, placement docs/rush2049_research/placement.md,
// collision docs/rush2049_research/collision.md, AI path docs/rush2049_research/race.md section 3.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#define TOML_EXCEPTIONS 1
#include "toml.hpp"

#include "gltf_scene.h"
#include "texture_upscale.h"
#include "track_mod.h"

namespace {
    using rush2::track_mod::Report;
    using Vec3 = std::array<float, 3>;

    constexpr float feet_per_meter = 3.2808399f;
    constexpr float section_size = 1024.0f;     // ft: visual sections (models) on a grid
    constexpr float vertex_reach = 2000.0f;     // ft: s16 vertices at 1/16 ft reach 2047 ft from their record
    constexpr float spine_step = 20.0f;         // ft between AI path points (the stock files' spacing)
    constexpr int max_checkpoints = 9;          // 10 checkpoint records, the finish line one of them
    constexpr int leaf_limit = 12;              // polygons per quadtree leaf before it splits
    constexpr int min_cell = 32;                // ft: smallest quadtree cell

    // Render state of Rush 2's own track sections (most common in HAWAII's lists).
    constexpr uint32_t mode_opaque = 0xC8112230;   // fog, then AA z-buffered opaque surface
    constexpr uint32_t mode_cutout = 0xC8113278;   // fog, then AA z-buffered alpha-tested (texture edge)
    constexpr uint32_t combine[2] = { 0xFC127FFF, 0xFFFFF238 };

    Vec3 sub(const Vec3& a, const Vec3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    Vec3 add(const Vec3& a, const Vec3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    Vec3 mul(const Vec3& a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
    float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    Vec3 cross(const Vec3& a, const Vec3& b) {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }
    float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
    Vec3 normalize(const Vec3& a) { float l = length(a); return l > 1e-12f ? mul(a, 1.0f / l) : Vec3{ 0, 0, 0 }; }
    float dist_xz(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

    // glTF point / direction -> game.
    Vec3 to_game(const Vec3& p) { return { -p[0] * feet_per_meter, p[1] * feet_per_meter, p[2] * feet_per_meter }; }
    Vec3 dir_to_game(const Vec3& d) { return { -d[0], d[1], d[2] }; }

    void add8(std::vector<uint8_t>& d, uint8_t v) { d.push_back(v); }
    void add16(std::vector<uint8_t>& d, uint16_t v) { d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v)); }
    void add32(std::vector<uint8_t>& d, uint32_t v) { for (int s = 24; s >= 0; s -= 8) d.push_back(uint8_t(v >> s)); }
    void addf(std::vector<uint8_t>& d, float f) { uint32_t u; memcpy(&u, &f, 4); add32(d, u); }
    void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (24 - 8 * i)); }
    void align8(std::vector<uint8_t>& d) { while (d.size() % 8) d.push_back(0); }
    void add_name(std::vector<uint8_t>& d, const std::string& n) {
        for (int i = 0; i < 16; i++) d.push_back(i < (int)n.size() && i < 15 ? uint8_t(n[i]) : 0);
    }

    // "COL_ROAD_main.001" -> "COL_ROAD_main".
    std::string base_name(const std::string& n) {
        size_t dot_at = n.rfind('.');
        if (dot_at != std::string::npos && dot_at + 1 < n.size() &&
            std::all_of(n.begin() + dot_at + 1, n.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
            return n.substr(0, dot_at);
        }
        return n;
    }
    std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }
    bool starts(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
    // "CHECKPOINT_12" with prefix "CHECKPOINT_" -> 12, else -1.
    int number_after(const std::string& s, const std::string& p) {
        if (!starts(s, p) || s.size() == p.size() || s.size() - p.size() > 6) return -1;
        for (size_t i = p.size(); i < s.size(); i++) if (!std::isdigit((unsigned char)s[i])) return -1;
        return std::stoi(s.substr(p.size()));
    }

    std::string fmt_ft(float ft) {
        char b[64];
        snprintf(b, sizeof(b), "%.0f m (%.0f ft)", ft / feet_per_meter, ft);
        return b;
    }
    std::string fmt_pos(const Vec3& g) { // a game point as the author's Blender coordinates (meters, Z up)
        char b[96];
        snprintf(b, sizeof(b), "(%.1f, %.1f, %.1f)", -g[0] / feet_per_meter, -g[2] / feet_per_meter, g[1] / feet_per_meter);
        return b;
    }

    // ---- collision -----------------------------------------------------------------------------------------------
    struct CollisionTri {
        Vec3 v[3];          // game space
        Vec3 front;         // the author's front-face normal, game space
        uint16_t flags;     // type | material << 4
        std::string object;
    };

    struct BuiltPoly {
        uint16_t flags;
        int16_t m[9];
        Vec3 v0, l1, l2;    // origin (world) and two local vertices
        float x0, x1, z0, z1;
    };

    void encode_vert(std::vector<uint8_t>& d, const Vec3& v) {
        int32_t q[3];
        for (int k = 0; k < 3; k++) q[k] = int32_t(std::floor(v[k] * 32.0f + 0.5f));
        for (int k = 0; k < 3; k++) add16(d, uint16_t(int16_t(q[k] >> 5)));
        add16(d, uint16_t(((q[0] & 31) << 10) | ((q[1] & 31) << 5) | (q[2] & 31)));
    }

    bool build_collision(const std::vector<CollisionTri>& tris, std::vector<uint8_t>& out, Report& report,
                         std::vector<BuiltPoly>* polys_out) {
        std::vector<BuiltPoly> polys;
        size_t flipped = 0, degenerate = 0;
        std::map<std::string, size_t> flipped_by;
        for (const CollisionTri& t : tris) {
            int type = t.flags & 0xF;
            bool ground = type != 5 && type != 6;
            Vec3 n = normalize(cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0])));
            if (length(n) < 0.5f) { degenerate++; continue; }
            if (dot(n, t.front) < 0) n = mul(n, -1.0f);       // the face's own front
            if (ground && n[1] < 0) { n = mul(n, -1.0f); flipped++; flipped_by[t.object]++; }
            Vec3 v[3] = { t.v[0], t.v[1], t.v[2] };
            // Local frame: x along the first edge, y the normal, z = x cross y; the vertices must wind so that the
            // third has local z > 0 (every stock polygon does: func_8006C670's edge tests).
            Vec3 x = normalize(sub(v[1], v[0]));
            Vec3 z = cross(x, n);
            if (dot(sub(v[2], v[0]), z) < 0) {
                std::swap(v[1], v[2]);
                x = normalize(sub(v[1], v[0]));
                z = cross(x, n);
            }
            Vec3 y = normalize(cross(z, x));
            z = normalize(z);
            BuiltPoly p{};
            p.flags = t.flags;
            const Vec3* rows[3] = { &x, &y, &z };
            float mq[9];
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                p.m[r * 3 + c] = int16_t(std::clamp(std::lround((*rows[r])[c] * 16384.0f), -32767l, 32767l));
                mq[r * 3 + c] = p.m[r * 3 + c] / 16384.0f;
            }
            p.v0 = v[0];
            auto local = [&](const Vec3& w) {
                Vec3 d = sub(w, v[0]);
                return Vec3{ mq[0] * d[0] + mq[1] * d[1] + mq[2] * d[2], 0.0f, mq[6] * d[0] + mq[7] * d[1] + mq[8] * d[2] };
            };
            p.l1 = local(v[1]);
            p.l2 = local(v[2]);
            p.x0 = std::min({ v[0][0], v[1][0], v[2][0] });
            p.x1 = std::max({ v[0][0], v[1][0], v[2][0] });
            p.z0 = std::min({ v[0][2], v[1][2], v[2][2] });
            p.z1 = std::max({ v[0][2], v[1][2], v[2][2] });
            polys.push_back(p);
        }
        if (degenerate) report.info(std::to_string(degenerate) + " collision triangles have no area and were skipped.");
        for (const auto& [obj, n] : flipped_by) {
            report.warning(obj + ": " + std::to_string(n) + " drivable faces point down (flipped normals). They were turned "
                           "up; recalculate normals in Blender (Mesh > Normals > Recalculate Outside) to be sure.");
        }
        if (polys.empty()) { report.error("No collision: add COL_ROAD_* meshes (or a visible track mesh)."); return false; }
        if (polys.size() > 8191) {
            report.error("Collision has " + std::to_string(polys.size()) + " triangles; the game's limit is 8191. "
                         "Simplify the COL_* meshes.");
            return false;
        }
        for (const BuiltPoly& p : polys) {
            for (const Vec3& v : { p.v0 }) {
                for (float c : v) if (std::fabs(c) > 32000.0f) {
                    report.error("Collision reaches " + fmt_pos(p.v0) + ", farther than the game allows "
                                 "(about 9,750 m from the origin).");
                    return false;
                }
            }
        }

        // Quadtree over x/z (func_8006CD60): q = (x >= mid) + 2 * (z < mid); the halves share the mid line.
        struct Node { int16_t parent; uint8_t mask; int16_t b[4]; uint16_t child[4]; };
        std::vector<Node> nodes;
        std::vector<uint8_t> leaves(1, 0);              // byte 0 pads so offset 0 means empty
        std::map<std::vector<uint8_t>, uint16_t> leaf_at;
        bool overflow = false;
        auto encode_leaf = [&](const std::vector<int>& list) -> uint16_t {
            if (list.empty()) return 0;
            std::vector<uint8_t> raw;
            raw.push_back(uint8_t(list.size()));
            for (size_t i = 0; i < list.size();) {
                size_t j = i + 1;
                while (j < list.size() && list[j] == list[j - 1] + 1 && j - i < 8) j++;
                add16(raw, uint16_t(((j - i - 1) << 13) | list[i]));
                i = j;
            }
            auto it = leaf_at.find(raw);
            if (it != leaf_at.end()) return it->second;
            if (leaves.size() + raw.size() > 0xFFFF) { overflow = true; return 0; }
            uint16_t off = uint16_t(leaves.size());
            leaves.insert(leaves.end(), raw.begin(), raw.end());
            leaf_at[raw] = off;
            return off;
        };
        auto overlaps = [&](const BuiltPoly& p, int x0, int x1, int z0, int z1) {
            // Inclusive and a foot wide: the game's cell is floor(x) (and x - 0.99 for negatives).
            return p.x1 >= x0 - 1 && p.x0 <= x1 + 1 && p.z1 >= z0 - 1 && p.z0 <= z1 + 1;
        };
        size_t biggest = 0;
        std::function<int(int, int, int, int, int, const std::vector<int>&)> make_node =
            [&](int parent, int x0, int x1, int z0, int z1, const std::vector<int>& list) -> int {
            int me = int(nodes.size());
            nodes.push_back(Node{ int16_t(parent), 0, { int16_t(x0), int16_t(x1), int16_t(z0), int16_t(z1) }, { 0, 0, 0, 0 } });
            int mx = (x0 + x1) >> 1, mz = (z0 + z1) >> 1;
            for (int q = 0; q < 4; q++) {
                int cx0 = (q & 1) ? mx : x0, cx1 = (q & 1) ? x1 : mx;
                int cz0 = (q & 2) ? z0 : mz, cz1 = (q & 2) ? mz : z1;
                std::vector<int> sub_list;
                for (int i : list) if (overlaps(polys[i], cx0, cx1, cz0, cz1)) sub_list.push_back(i);
                bool split = (int)sub_list.size() > leaf_limit && cx1 - cx0 > min_cell && cz1 - cz0 > min_cell;
                if (split) {
                    int child = make_node(me, cx0, cx1, cz0, cz1, sub_list);
                    nodes[me].mask |= uint8_t(1 << q);
                    nodes[me].child[q] = uint16_t(child);
                }
                else {
                    if (sub_list.size() > 255) sub_list.resize(255), overflow = true;
                    biggest = std::max(biggest, sub_list.size());
                    nodes[me].child[q] = encode_leaf(sub_list);
                }
            }
            return me;
        };
        float bx0 = 1e30f, bx1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
        for (const BuiltPoly& p : polys) {
            bx0 = std::min(bx0, p.x0); bx1 = std::max(bx1, p.x1); bz0 = std::min(bz0, p.z0); bz1 = std::max(bz1, p.z1);
        }
        std::vector<int> all(polys.size());
        for (size_t i = 0; i < polys.size(); i++) all[i] = int(i);
        make_node(-1, int(std::floor(bx0)) - 2, int(std::ceil(bx1)) + 2, int(std::floor(bz0)) - 2, int(std::ceil(bz1)) + 2, all);
        if (overflow || nodes.size() > 0x7FFF) {
            report.error("The collision is too dense for the game's collision index (more than 64 KB of cell lists, or "
                         "over 255 triangles in one small spot). Simplify the COL_* meshes.");
            return false;
        }

        std::vector<uint8_t> vlist;
        for (size_t i = 0; i < polys.size(); i++) {
            add16(vlist, uint16_t(i * 3));
            add8(vlist, 0xC2);                        // run: the next two indices follow
        }
        out.clear();
        for (uint16_t v : { uint16_t(0), uint16_t(nodes.size()), uint16_t(polys.size()), uint16_t(polys.size() * 3),
                            uint16_t(leaves.size()), uint16_t(vlist.size()) }) {
            add16(out, v);
        }
        for (const Node& n : nodes) {
            add16(out, uint16_t(n.parent));
            add8(out, 0);
            add8(out, n.mask);
            for (int16_t b : n.b) add16(out, uint16_t(b));
            for (uint16_t c : n.child) add16(out, c);
        }
        for (size_t i = 0; i < polys.size(); i++) {
            add16(out, polys[i].flags);
            add16(out, 3);                            // info: 3 vertices
            for (int16_t m : polys[i].m) add16(out, uint16_t(m));
            add16(out, uint16_t(i * 3));              // vertex list offset
        }
        for (const BuiltPoly& p : polys) {
            encode_vert(out, p.v0);
            encode_vert(out, p.l1);
            encode_vert(out, p.l2);
        }
        out.insert(out.end(), leaves.begin(), leaves.end());
        out.insert(out.end(), vlist.begin(), vlist.end());
        char b[160];
        snprintf(b, sizeof(b), "Collision: %zu triangles, %zu index cells (largest %zu), %zu bytes.", polys.size(),
                 nodes.size(), biggest, out.size());
        report.info(b);
        if (flipped) report.info(std::to_string(flipped) + " drivable collision faces were turned to face up.");
        if (polys_out) *polys_out = std::move(polys);
        return true;
    }

    // Height of the drivable ground under (x, z) within `above`/`below` ft of y, or NaN.
    float ground_under(const std::vector<BuiltPoly>& polys, const Vec3& p, float reach) {
        float best = NAN, best_d = 1e30f;
        for (const BuiltPoly& q : polys) {
            int type = q.flags & 0xF;
            if (type == 5 || type == 6 || type == 7) continue;
            if (p[0] < q.x0 - 0.5f || p[0] > q.x1 + 0.5f || p[2] < q.z0 - 0.5f || p[2] > q.z1 + 0.5f) continue;
            float m[9];
            for (int i = 0; i < 9; i++) m[i] = q.m[i] / 16384.0f;
            Vec3 d = sub(p, q.v0);
            float lx = m[0] * d[0] + m[1] * d[1] + m[2] * d[2], ly = m[3] * d[0] + m[4] * d[1] + m[5] * d[2];
            float lz = m[6] * d[0] + m[7] * d[1] + m[8] * d[2];
            // point in triangle (0,0), l1, l2 in local x/z
            float ax = 0, az = 0, bx = q.l1[0], bz = q.l1[2], cx = q.l2[0], cz = q.l2[2];
            auto side = [](float x0, float z0, float x1, float z1, float px, float pz) { return (x1 - x0) * (pz - z0) - (z1 - z0) * (px - x0); };
            float s0 = side(ax, az, bx, bz, lx, lz), s1 = side(bx, bz, cx, cz, lx, lz), s2 = side(cx, cz, ax, az, lx, lz);
            bool in = (s0 >= -0.5f && s1 >= -0.5f && s2 >= -0.5f) || (s0 <= 0.5f && s1 <= 0.5f && s2 <= 0.5f);
            if (!in) continue;
            // height of the plane at (x, z): move along world y until local y = 0
            float h = std::fabs(m[4]) > 1e-4f ? p[1] - ly / m[4] : NAN;
            if (std::isnan(h)) continue;
            float dd = std::fabs(h - p[1]);
            if (dd < reach && dd < best_d) { best_d = dd; best = h; }
        }
        return best;
    }

    // ---- textures ------------------------------------------------------------------------------------------------
    // An RGBA16 stand-in (4 KB of TMEM at most) of a material's image tinted by its color. The game draws the full-size
    // image in its place (Texture::key, src/track_mod/track_mod.cpp publish_textures).
    rush2::track_mod::Texture make_texture(const rush2::gltf::Scene& scene, const rush2::gltf::Material* mat, float cutoff) {
        rush2::track_mod::Texture t;
        t.cutoff = cutoff;
        rush2::upscale::Image image;
        if (mat && mat->image >= 0) {
            t.image = mat->image;
            rush2::upscale::decode_image(scene.images[mat->image].data, image);
        }
        if (image.empty()) {
            image.width = image.height = 1;
            image.rgba = { 255, 255, 255, 255 };
        }
        float aspect = float(image.width) / float(image.height);
        t.width = aspect >= 1.5f ? 64 : 32;
        t.height = aspect <= 0.67f ? 64 : 32;
        if (image.width <= t.width && image.height <= t.height && image.width >= 4 && image.height >= 4 &&
            (image.width & (image.width - 1)) == 0 && (image.height & (image.height - 1)) == 0) {
            t.width = image.width; t.height = image.height;
        }
        rush2::upscale::Image small = (image.width == t.width && image.height == t.height) ? image : rush2::upscale::shrink(image, t.width, t.height);
        std::array<float, 4> tint = mat ? mat->color : std::array<float, 4>{ 1, 1, 1, 1 };
        t.tint = tint;
        t.texels.resize(size_t(t.width) * t.height * 2);
        for (size_t i = 0; i < size_t(t.width) * t.height; i++) {
            uint8_t q[4];
            for (int k = 0; k < 4; k++) q[k] = uint8_t(std::clamp(small.rgba[i * 4 + k] * tint[k], 0.0f, 255.0f));
            uint16_t c = uint16_t(((q[0] >> 3) << 11) | ((q[1] >> 3) << 6) | ((q[2] >> 3) << 1) | (q[3] >= cutoff * 255.0f ? 1 : 0));
            t.texels[i * 2] = uint8_t(c >> 8);
            t.texels[i * 2 + 1] = uint8_t(c);
        }
        t.key = rush2::upscale::rgba16_key(t.texels, t.width, t.height);
        return t;
    }

    // ---- visual geometry -----------------------------------------------------------------------------------------
    struct VisualTri {
        Vec3 p[3];          // game space, ft
        Vec3 n[3];          // game space
        std::array<float, 2> uv[3];
        std::array<float, 4> col[3];
        bool has_col = false;
        int material = -1;  // scene material
    };

    struct Section {
        std::string name;
        Vec3 origin{};
        std::vector<int> tris;
        Vec3 lo{}, hi{};
        float radius = 0.0f;
        uint32_t list = 0;
    };

    // A material as the lists draw it.
    struct DrawMaterial {
        const rush2::track_mod::Texture* texture = nullptr;
        bool cutout = false;
        bool wraps = true;      // repeats (not clamped) both ways: coordinates may be shifted by whole repeats
    };

    // A list being built: its commands, the vertices they load (offsets into `vertices` in the G_VTX commands listed
    // in vtx_commands), and the extent of what it draws (relative to its origin, ft).
    struct MeshList {
        std::vector<uint8_t> vertices;
        std::vector<std::pair<uint32_t, uint32_t>> list;
        std::vector<size_t> vtx_commands;
        Vec3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        float radius = 0.0f;
        void cmd(uint32_t w0, uint32_t w1) { list.emplace_back(w0, w1); }
    };

    // Appends triangles `tris` of vtris, grouped by scene material in `order` (drawn as materials[i]), as batches of
    // G_VTX (at most 32 vertices) and G_TRI2. Vertices are (p - origin) * unit; load(m, i) emits material i's texture
    // load. Meshes without vertex colors get the shading Rush 2 bakes into its tracks: a fixed light from above.
    void emit_mesh(const std::vector<VisualTri>& vtris, const std::vector<int>& tris, const std::vector<int>& order,
                   const std::vector<DrawMaterial>& materials, const Vec3& origin, float unit,
                   const std::function<void(MeshList&, size_t)>& load, MeshList& m) {
        const float light[3] = { 0.30f, 0.80f, 0.52f };
        int current_mode = -1;
        for (size_t mi = 0; mi < order.size(); mi++) {
            std::vector<int> mine;
            for (int t : tris) if (vtris[t].material == order[mi]) mine.push_back(t);
            if (mine.empty()) continue;
            const DrawMaterial& dm = materials[mi];
            const rush2::track_mod::Texture& tex = *dm.texture;
            int mode = dm.cutout ? 1 : 0;
            if (mode != current_mode) { m.cmd(0xE200001C, mode ? mode_cutout : mode_opaque); current_mode = mode; }
            load(m, mi);
            // Each triangle's texture coordinates are shifted by whole repeats to stay inside the s16 range.
            std::vector<uint8_t> batch;
            std::vector<std::array<int, 3>> batch_tris;
            int slots = 0;
            auto flush = [&]() {
                if (slots == 0) return;
                uint32_t at = uint32_t(m.vertices.size());
                m.vertices.insert(m.vertices.end(), batch.begin(), batch.end());
                m.vtx_commands.push_back(m.list.size());
                m.cmd(0x01000000 | (uint32_t(slots) << 12) | (uint32_t(slots) << 1), at);
                for (size_t t = 0; t < batch_tris.size(); t += 2) {
                    uint32_t a = (batch_tris[t][0] * 2 << 16) | (batch_tris[t][1] * 2 << 8) | (batch_tris[t][2] * 2);
                    if (t + 1 < batch_tris.size()) {
                        uint32_t b = (batch_tris[t + 1][0] * 2 << 16) | (batch_tris[t + 1][1] * 2 << 8) | (batch_tris[t + 1][2] * 2);
                        m.cmd(0x06000000 | a, b);
                    }
                    else m.cmd(0x05000000 | a, 0);
                }
                batch.clear();
                batch_tris.clear();
                slots = 0;
            };
            for (int ti : mine) {
                const VisualTri& t = vtris[ti];
                if (slots + 3 > 32) flush();
                float su = 0, sv = 0;
                if (dm.wraps) {
                    su = std::floor(std::min({ t.uv[0][0], t.uv[1][0], t.uv[2][0] }));
                    sv = std::floor(std::min({ t.uv[0][1], t.uv[1][1], t.uv[2][1] }));
                }
                std::array<int, 3> tri;
                for (int k = 0; k < 3; k++) {
                    Vec3 rel = sub(t.p[k], origin);
                    for (int c = 0; c < 3; c++) { m.lo[c] = std::min(m.lo[c], rel[c]); m.hi[c] = std::max(m.hi[c], rel[c]); }
                    m.radius = std::max(m.radius, length(rel));
                    int16_t xyz[3];
                    for (int c = 0; c < 3; c++) xyz[c] = int16_t(std::clamp(std::lround(rel[c] * unit), -32767l, 32767l));
                    uint8_t rgba[4];
                    if (t.has_col) {
                        for (int c = 0; c < 4; c++) rgba[c] = uint8_t(std::clamp(t.col[k][c] * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                    else {
                        const Vec3& nn = t.n[k];
                        float shade = 0.72f + 0.28f * std::max(0.0f, nn[0] * light[0] + nn[1] * light[1] + nn[2] * light[2]);
                        rgba[0] = rgba[1] = rgba[2] = uint8_t(std::clamp(shade * 255.0f, 0.0f, 255.0f));
                        rgba[3] = 255;
                    }
                    // N64 texel centers are at whole coordinates (src/car_mods.cpp emit_body).
                    int32_t st[2] = { int32_t(std::lround(((t.uv[k][0] - su) * tex.width - 0.5f) * 32.0f)),
                                      int32_t(std::lround(((t.uv[k][1] - sv) * tex.height - 0.5f) * 32.0f)) };
                    for (int c = 0; c < 3; c++) add16(batch, uint16_t(xyz[c]));
                    add16(batch, 0);
                    for (int c = 0; c < 2; c++) add16(batch, uint16_t(int16_t(std::clamp(st[c], -32767, 32767))));
                    batch.insert(batch.end(), rgba, rgba + 4);
                    tri[k] = slots++;
                }
                batch_tris.push_back(tri);
            }
            flush();
        }
    }

    // The commands loading RGBA16 stand-in texture t from `texels` (the pointer is the first command's second word),
    // with the material's wrap modes.
    std::vector<std::pair<uint32_t, uint32_t>> texture_load(const rush2::track_mod::Texture& t,
                                                           const rush2::gltf::Material* mat, uint32_t texels) {
        auto mode = [](int wrap) { return wrap == 33648 ? 1u : wrap == 33071 ? 2u : 0u; }; // mirror / clamp
        auto mask = [](uint32_t n) { return n == 64 ? 6u : n == 32 ? 5u : n == 16 ? 4u : n == 8 ? 3u : 2u; };
        uint32_t cms = mat ? mode(mat->wrap_s) : 0, cmt = mat ? mode(mat->wrap_t) : 0;
        uint32_t tile = (cmt << 18) | (mask(t.height) << 14) | (cms << 8) | (mask(t.width) << 4);
        uint32_t line = t.width * 2 / 8;
        return { { 0xFD100000u, texels }, { 0xF5100000u, 0x07000000u | tile }, { 0xE6000000u, 0u },
                 { 0xF3000000u, 0x07000000u | ((t.width * t.height - 1) << 12) | (2048 / line) }, { 0xE7000000u, 0u },
                 { 0xF5100000u | (line << 9), tile }, { 0xF2000000u, (((t.width - 1) * 4) << 12) | ((t.height - 1) * 4) } };
    }

    // ---- AI path ---------------------------------------------------------------------------------------------------
    struct PathPoint { Vec3 p; Vec3 lateral; float half_width; };

    // A node's triangles in game space.
    void append_tris(const rush2::gltf::Node& n, std::vector<VisualTri>& out) {
        for (const auto& prim : n.primitives) {
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                VisualTri v;
                for (int k = 0; k < 3; k++) {
                    uint32_t i = prim.tris[t + k];
                    v.p[k] = to_game(prim.pos[i]);
                    v.n[k] = dir_to_game(prim.nrm[i]);
                    v.uv[k] = prim.uv[i];
                    if (!prim.col.empty()) { v.col[k] = prim.col[i]; v.has_col = true; }
                }
                v.material = prim.material;
                out.push_back(v);
            }
        }
    }

    // ---- track select diorama ----------------------------------------------------------------------------------------
    // The author's diorama.glb (dioramas are authored: a miniature generated from the track doesn't read well, see
    // src/rush2049/track2049_art.cpp use_miniature), fitted the way the stock dioramas in asset 3 are built (measured:
    // VEGASTRACK..STUNTTRACK span 1000-1280 model units on their longer side, centered on x and z, from y -80 (the
    // base's bottom) up). The stock ones are shown at scales that make about 800 units of their longer side; the
    // track select shows these at 2/3 (src/rush2049/track2049_menu.cpp).
    constexpr float diorama_side = 1200.0f;
    constexpr float diorama_floor = -80.0f;
    constexpr float diorama_triangle_budget = 4000;     // stock dioramas: 750-2400 vertices

    // Builds art.diorama from `tris` (materials of `scene`); its stand-in textures are added to kept.
    bool build_diorama(std::vector<rush2::track_mod::Texture>& kept, const rush2::gltf::Scene& scene,
                       const std::vector<VisualTri>& tris, const std::string& what,
                       rush2::track_mod::MenuArt& art, Report& report) {
        if (tris.empty()) {
            report.warning(what + " has no triangles; the track select shows no diorama.");
            return false;
        }
        Vec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
        for (const VisualTri& t : tris) for (const Vec3& p : t.p) for (int c = 0; c < 3; c++) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
        float side = std::max({ hi[0] - lo[0], hi[2] - lo[2], 1.0f });
        float unit = diorama_side / side;                   // model units per ft
        Vec3 origin = { (lo[0] + hi[0]) * 0.5f, lo[1] - diorama_floor / unit, (lo[2] + hi[2]) * 0.5f };
        if ((hi[1] - origin[1]) * unit > 32000.0f) {
            report.warning(what + " is too tall for its footprint to make a diorama.");
            return false;
        }

        if (tris.size() > diorama_triangle_budget) {
            report.warning(what + " makes a diorama of " + std::to_string(tris.size()) + " triangles (the stock ones have "
                           "at most about 2400); the track select may slow down.");
        }

        // Textures: one stand-in per material.
        std::vector<int> order;
        for (const VisualTri& t : tris) if (std::find(order.begin(), order.end(), t.material) == order.end()) order.push_back(t.material);
        std::vector<rush2::track_mod::Texture> textures;
        std::vector<const rush2::gltf::Material*> mats;
        textures.reserve(order.size());
        for (int m : order) {
            const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
            mats.push_back(mat);
            bool cutout = mat && mat->alpha_mode != "OPAQUE";
            textures.push_back(make_texture(scene, mat, cutout ? (mat->alpha_mode == "MASK" ? mat->alpha_cutoff : 0.5f) : 0.0f));
        }
        std::vector<DrawMaterial> draw;
        for (size_t i = 0; i < order.size(); i++) {
            const rush2::gltf::Material* mat = mats[i];
            draw.push_back({ &textures[i], mat && mat->alpha_mode != "OPAQUE",
                             !mat || (mat->wrap_s != 33071 && mat->wrap_t != 33071) });
        }

        // Blob: texels, vertices, list.
        std::vector<uint8_t>& d = art.diorama;
        d.clear();
        art.diorama_pointers.clear();
        std::vector<uint32_t> texel_at;
        for (rush2::track_mod::Texture& t : textures) {
            align8(d);
            texel_at.push_back(uint32_t(d.size()));
            d.insert(d.end(), t.texels.begin(), t.texels.end());
        }
        MeshList m;
        std::vector<size_t> settimg;
        m.cmd(0xE7000000, 0);
        m.cmd(0xE3001001, 0x00000000);          // no TLUT: true color
        m.cmd(0xE3000F00, 0x00000000);          // no mipmaps
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);          // G_TEXTURE on, tile 0, scale 1
        std::vector<int> all(tris.size());
        for (size_t i = 0; i < tris.size(); i++) all[i] = int(i);
        // Inline loads: the loader doesn't relocate inside called lists (func_8007786C doesn't enter G_DL targets).
        emit_mesh(tris, all, order, draw, origin, unit, [&](MeshList& ml, size_t mi) {
            bool first = true;
            for (const auto& [w0, w1] : texture_load(textures[mi], mats[mi], texel_at[mi])) {
                if (first) settimg.push_back(ml.list.size());
                first = false;
                ml.cmd(w0, w1);
            }
        }, m);
        m.cmd(0xE3001001, 0x00008000);          // the TLUT mode Rush 2's lists leave set (RGBA16)
        m.cmd(0xDF000000, 0);
        align8(d);
        uint32_t base = uint32_t(d.size());
        d.insert(d.end(), m.vertices.begin(), m.vertices.end());
        align8(d);
        art.diorama_list = uint32_t(d.size());
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        std::set<size_t> pointers(m.vtx_commands.begin(), m.vtx_commands.end());
        pointers.insert(settimg.begin(), settimg.end());
        for (size_t i = 0; i < m.list.size(); i++) {
            const auto& [w0, w1] = m.list[i];
            if (pointers.count(i)) {
                art.diorama_pointers.push_back(uint32_t(d.size() + 4));
            }
            add32(d, w0);
            add32(d, w1);
        }
        art.diorama_radius = m.radius * unit / 16.0f;
        char b[160];
        snprintf(b, sizeof(b), "Diorama: %zu triangles from %s, %zu textures, %zu bytes.", tris.size(), what.c_str(),
                 textures.size(), d.size());
        report.info(b);
        for (rush2::track_mod::Texture& t : textures) {
            t.source = rush2::track_mod::Texture::diorama;
            kept.push_back(std::move(t));
        }
        return true;
    }
}

std::string rush2::track_mod::Report::text() const {
    std::string s;
    for (const Message& m : messages) {
        s += m.level == Message::error ? "ERROR: " : m.level == Message::warning ? "Warning: " : "";
        s += m.text;
        s += "\n";
    }
    return s;
}

bool rush2::track_mod::parse_settings(const std::string& text, Settings& s, Report& report) {
    toml::table t;
    try {
        t = toml::parse(text);
    }
    catch (const toml::parse_error& e) {
        std::ostringstream o;
        o << "track.toml line " << e.source().begin.line << ": " << e.description();
        report.error(o.str());
        return false;
    }
    auto str = [&](const char* sect, const char* key, std::string& dst) {
        if (auto v = t[sect][key].value<std::string>()) dst = *v;
    };
    str("track", "name", s.name);
    str("track", "author", s.author);
    str("track", "version", s.version);
    str("track", "description", s.description);
    if (s.name.empty()) report.error("track.toml: [track] name is required.");
    // Not track settings: the players pick the lap count and mirror mode, which every track has.
    if (t["race"]["laps"]) report.info("track.toml: laps is ignored (players pick the lap count).");
    if (t["race"]["mirror"]) report.info("track.toml: mirror is ignored (every track can be raced mirrored).");
    if (auto v = t["race"]["start_time"].value<double>()) s.start_time = float(*v);
    if (auto* arr = t["race"]["checkpoint_time"].as_array()) {
        for (auto& e : *arr) {
            std::array<float, 3> times{};
            auto* inner = e.as_array();
            // [first lap, later laps]: Rush 2 keeps those two per checkpoint. A third number (the old [lap 1, lap 2,
            // lap 3+] form) is read and ignored.
            if (!inner || inner->size() < 2 || inner->size() > 3) {
                report.error("track.toml: each checkpoint_time entry needs two numbers [first lap, later laps].");
                continue;
            }
            if (inner->size() == 3 && s.checkpoint_time.empty()) {
                report.info("track.toml: checkpoint_time's third number is ignored (Rush 2 has one time for every lap "
                            "after the first).");
            }
            for (size_t i = 0; i < inner->size(); i++) times[i] = float((*inner)[i].value<double>().value_or(0.0));
            s.checkpoint_time.push_back(times);
        }
    }
    if (auto* arr = t["race"]["best_times"].as_array()) {
        for (auto& e : *arr) s.best_times.push_back(float(e.value<double>().value_or(0.0)));
    }
    if (auto v = t["look"]["fog_color"].value<std::string>()) {
        unsigned r, g, b;
        if (v->size() == 7 && (*v)[0] == '#' && sscanf(v->c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            s.fog = { uint8_t(r), uint8_t(g), uint8_t(b) };
        }
        else {
            report.error("track.toml: fog_color must look like \"#a0b8d0\".");
        }
    }
    if (t["look"]["fog_colour"]) report.warning("track.toml: the key is fog_color (\"fog_colour\" is ignored).");
    str("look", "sky", s.sky);
    if (s.sky != "none") report.warning("track.toml: sky isn't supported yet; ignored.");
    if (auto v = t["look"]["cloud_scroll"].value<double>()) s.cloud_scroll = float(*v);
    if (auto v = t["look"]["sky_parallax"].value<double>()) s.sky_parallax = float(*v);
    str("audio", "music", s.music);
    str("menu", "preview", s.preview);
    str("menu", "logo", s.logo);
    str("menu", "diorama", s.diorama);
    if (auto* surf = t["surfaces"].as_table()) {
        int next_slot = 3;
        for (auto& [key, node] : *surf) {
            std::string name = upper(std::string(key.str()));
            Surface sf;
            auto* tbl = node.as_table();
            if (!tbl) { report.error("track.toml: [surfaces." + std::string(key.str()) + "] must be a table."); continue; }
            sf.like = (*tbl)["like"].value_or(std::string());
            if (sf.like != "road" && sf.like != "dirt" && sf.like != "grass") {
                report.error("track.toml: surfaces." + std::string(key.str()) + ".like must be \"road\", \"dirt\" or \"grass\".");
                continue;
            }
            sf.drag = float((*tbl)["drag"].value_or(0.0));
            if (name == "ROAD" || name == "DIRT" || name == "GRASS" || name == "WALL" || name == "WATER" || name == "KILL") {
                report.error("track.toml: surfaces." + std::string(key.str()) + " uses a built-in surface's name.");
                continue;
            }
            if (next_slot == 8) next_slot++;        // 8 is the game's "airborne" marker
            if (next_slot > 15) { report.error("track.toml: at most 12 custom surfaces."); break; }
            sf.material = next_slot++;
            s.surfaces[name] = sf;
        }
    }
    return report.ok();
}

bool rush2::track_mod::convert(const std::vector<uint8_t>& glb, const std::string& toml_text, const std::string& prefix,
                               Converted& out, Report& report, const Extras& extras) {
    out = Converted{};
    parse_settings(toml_text, out.settings, report);
    const Settings& settings = out.settings;

    rush2::gltf::Scene scene;
    std::string error;
    if (!rush2::gltf::read_glb(glb, scene, error)) { report.error("track.glb: " + error); return false; }

    // ---- sort the objects ----
    const rush2::gltf::Node* finish = nullptr;
    const rush2::gltf::Node* aipath = nullptr;
    std::map<int, const rush2::gltf::Node*> checkpoints, grid;
    std::vector<std::pair<float, const rush2::gltf::Node*>> aispeeds;
    std::vector<const rush2::gltf::Node*> visual;
    std::vector<std::pair<uint16_t, const rush2::gltf::Node*>> collision_nodes;
    std::set<std::string> seen;
    for (const rush2::gltf::Node& n : scene.nodes) {
        std::string name = base_name(n.name);
        bool mesh = !n.primitives.empty();
        if (!seen.insert(name).second && (name == "FINISH" || name == "AIPATH_main" || starts(name, "CHECKPOINT_"))) {
            report.error("There are two objects named " + name + ".");
            continue;
        }
        if (name.find('*') != std::string::npos) {
            report.warning("\"" + n.name + "\": a * in a name stands for any text (COL_ROAD_main), it isn't typed "
                           "literally. Read as \"" + name.substr(0, name.find('*')) + "...\".");
        }
        if (starts(name, "COL_")) {
            std::string rest = name.substr(4);
            // The longest surface name that the rest starts with (followed by its end or "_").
            struct Known { std::string name; uint16_t flags; };
            std::vector<Known> known = { { "ROAD", 0x00 }, { "DIRT", 0x11 }, { "GRASS", 0x22 }, { "WALL", 0x05 },
                                         { "WATER", 0x03 }, { "KILL", 0x07 } };
            for (const auto& [sname, sf] : settings.surfaces) {
                uint16_t type = sf.like == "road" ? 0 : sf.like == "dirt" ? 1 : 2;
                known.push_back({ sname, uint16_t(type | (sf.material << 4)) });
            }
            const Known* best = nullptr;
            std::string urest = upper(rest);
            for (const Known& k : known) {
                if (starts(urest, k.name) && (urest.size() == k.name.size() || urest[k.name.size()] == '_' || urest[k.name.size()] == '*') &&
                    (!best || k.name.size() > best->name.size())) {
                    best = &k;
                }
            }
            if (!best) {
                report.error(n.name + ": unknown surface. Use COL_ROAD, COL_DIRT, COL_GRASS, COL_WALL, COL_WATER, COL_KILL "
                             "or a surface named in track.toml's [surfaces].");
                continue;
            }
            if (!starts(rest, best->name)) report.warning(n.name + ": surface names are upper case (COL_" + best->name + "_...).");
            if (!mesh) { report.warning(n.name + " has no mesh; ignored."); continue; }
            collision_nodes.push_back({ best->flags, &n });
            continue;
        }
        if (name == "AIPATH_main") { aipath = &n; continue; }
        if (starts(name, "AIPATH")) { report.warning(n.name + ": only one AI path, named AIPATH_main, is supported."); continue; }
        if (name == "FINISH") { finish = &n; continue; }
        if (int k = number_after(name, "CHECKPOINT_"); k >= 0) { checkpoints[k] = &n; continue; }
        if (int k = number_after(name, "START_"); k >= 0) { grid[k] = &n; continue; }
        if (int k = number_after(name, "AISPEED_"); k >= 0) { aispeeds.push_back({ float(k), &n }); continue; }
        if (name == "SKY") { report.warning("SKY: sky models aren't supported yet; it is drawn as part of the track."); visual.push_back(&n); continue; }
        if (starts(name, "PROP_")) { report.warning(n.name + ": props aren't supported yet; ignored."); continue; }
        if (mesh) { visual.push_back(&n); continue; }
        if (n.loose_edges) { report.warning(n.name + " is only lines (loose edges); ignored."); continue; }
        if (!n.name.empty()) report.info(n.name + " isn't a recognized marker; ignored.");
    }
    if (!grid.empty()) {
        report.info("START_1-" + std::to_string(grid.rbegin()->first) + " are ignored: Rush 2 lines the 8 cars up "
                    "itself, two per row, rows 20 ft (6 m) apart, from the finish line back.");
    }

    // ---- markers ----
    auto forward = [](const rush2::gltf::Node* n) {   // the green (Blender Y = glTF -Z) arrow, flat
        Vec3 f = dir_to_game(mul(n->axis(2), -1.0f));
        f[1] = 0.0f;
        return normalize(f);
    };
    auto half_width = [](const rush2::gltf::Node* n) { return length(n->axis(0)) * feet_per_meter; };
    if (!finish) report.error("No FINISH marker: add an Empty named FINISH on the start/finish line.");
    if (checkpoints.empty()) report.error("No checkpoints: add Empties CHECKPOINT_1, CHECKPOINT_2, ... around the lap.");
    int expected = 1;
    for (const auto& [k, n] : checkpoints) {
        if (k != expected) report.error("Checkpoints must be numbered 1, 2, 3, ... without gaps; CHECKPOINT_" +
                                        std::to_string(expected) + " is missing.");
        expected = k + 1;
    }
    if ((int)checkpoints.size() > max_checkpoints) {
        report.error("At most " + std::to_string(max_checkpoints) + " checkpoints (the game has 10 gates including the finish).");
    }
    struct Gate { std::string name; Vec3 pos, dir; float radius; };
    std::vector<Gate> gates;
    if (finish) gates.push_back({ "FINISH", to_game(finish->position()), forward(finish), half_width(finish) });
    for (const auto& [k, n] : checkpoints) {
        gates.push_back({ "CHECKPOINT_" + std::to_string(k), to_game(n->position()), forward(n), half_width(n) });
    }
    for (size_t i = 0; i < gates.size(); i++) {
        const Gate& g = gates[i];
        if (length(g.dir) < 0.5f) report.error(g.name + " points straight up or down; turn it so its green arrow points down the track.");
        if (g.radius < 3.0f) report.error(g.name + " is " + fmt_ft(g.radius * 2) + " wide; scale its X to half the road's width.");
        for (size_t j = 0; j < i; j++) {
            if (dist_xz(gates[j].pos, g.pos) < 1.0f) {
                report.error(g.name + " and " + gates[j].name + " are in the same place " + fmt_pos(g.pos) +
                             (dist_xz(g.pos, { 0, 0, 0 }) < 1.0f ? " (the origin: was it moved into place?)" : "") + ".");
            }
        }
    }
    if (!settings.checkpoint_time.empty() && settings.checkpoint_time.size() != checkpoints.size()) {
        report.error("track.toml: checkpoint_time has " + std::to_string(settings.checkpoint_time.size()) +
                     " entries but there are " + std::to_string(checkpoints.size()) + " checkpoints (one entry each).");
    }

    // ---- collision ----
    std::vector<CollisionTri> ctris;
    auto add_collision = [&](const rush2::gltf::Node* n, uint16_t flags) {
        for (const auto& prim : n->primitives) {
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                CollisionTri c;
                for (int k = 0; k < 3; k++) c.v[k] = to_game(prim.pos[prim.tris[t + k]]);
                // glTF front = counter-clockwise; x negated mirrors it, so the front normal is the game cross's negation.
                c.front = mul(cross(sub(c.v[1], c.v[0]), sub(c.v[2], c.v[0])), -1.0f);
                c.flags = flags;
                c.object = n->name;
                ctris.push_back(c);
            }
        }
    };
    for (const auto& [flags, n] : collision_nodes) add_collision(n, flags);
    if (collision_nodes.empty()) {
        report.warning("No COL_* meshes: the visible track is used as road collision (walls included).");
        for (const auto* n : visual) add_collision(n, 0x00);
    }
    std::vector<BuiltPoly> cpolys;
    build_collision(ctris, out.track.collision, report, &cpolys);

    // ---- AI path ----
    std::vector<PathPoint> rungs;
    if (!aipath) {
        report.error("No AIPATH_main: draw the racing line as a curve named AIPATH_main with Geometry > Extrude set to "
                     "about half the road's width (a curve with no Extrude isn't exported).");
    }
    else if (aipath->primitives.empty()) {
        report.error("AIPATH_main has no surface: give the curve Geometry > Extrude (about half the road's width) so it is "
                     "exported.");
    }
    else {
        const auto& prim = aipath->primitives[0];
        if (aipath->primitives.size() > 1) report.warning("AIPATH_main has several parts; only the first is used.");
        if (prim.pos.size() < 8 || prim.pos.size() % 2) {
            report.error("AIPATH_main isn't a ribbon: use a curve with Extrude (no Bevel depth), not a mesh.");
        }
        else {
            for (size_t i = 0; i + 1 < prim.pos.size(); i += 2) {
                Vec3 a = to_game(prim.pos[i]), b = to_game(prim.pos[i + 1]);
                Vec3 lat = sub(b, a);
                lat[1] = 0.0f;
                rungs.push_back({ mul(add(a, b), 0.5f), normalize(lat), length(lat) * 0.5f });
            }
            std::vector<float> gaps;
            for (size_t i = 1; i < rungs.size(); i++) gaps.push_back(length(sub(rungs[i].p, rungs[i - 1].p)));
            std::vector<float> sorted_gaps = gaps;
            std::sort(sorted_gaps.begin(), sorted_gaps.end());
            float median = sorted_gaps[sorted_gaps.size() / 2];
            float worst = sorted_gaps.back();
            if (median < 0.01f || worst > median * 20.0f + 50.0f) {
                report.error("AIPATH_main's points aren't in order along the path (was it converted to a mesh and edited?). "
                             "Use the curve itself, with Extrude.");
                rungs.clear();
            }
            else if (rungs.front().half_width < 1.0f) {
                report.error("AIPATH_main's Extrude is too small; set it to about half the road's width.");
                rungs.clear();
            }
            else {
                float closing = length(sub(rungs.front().p, rungs.back().p));
                if (closing > median * 3.0f + 10.0f) {
                    report.warning("AIPATH_main isn't a closed loop (its ends are " + fmt_ft(closing) +
                                   " apart); it is closed with a straight line. In Blender: Curve > Toggle Cyclic.");
                }
            }
        }
    }

    std::vector<PathPoint> spine;   // every spine_step ft, starting at the finish line
    if (rungs.size() >= 4 && finish) {
        // Race direction: the path should run the way FINISH points.
        const Gate& fg = gates[0];
        size_t near = 0;
        float best = 1e30f;
        for (size_t i = 0; i < rungs.size(); i++) {
            float d = dist_xz(rungs[i].p, fg.pos);
            if (d < best) { best = d; near = i; }
        }
        Vec3 along = sub(rungs[(near + 1) % rungs.size()].p, rungs[near].p);
        if (dot(along, fg.dir) < 0) {
            std::reverse(rungs.begin(), rungs.end());
            for (PathPoint& r : rungs) r.lateral = mul(r.lateral, -1.0f);
            near = rungs.size() - 1 - near;
            report.info("AIPATH_main runs against FINISH's direction; it was reversed.");
        }
        if (best > fg.radius + 20.0f) {
            report.error("AIPATH_main doesn't pass through FINISH (closest " + fmt_ft(best) + " away).");
        }
        // Start at the rung nearest the finish line, wrap around, and resample.
        std::rotate(rungs.begin(), rungs.begin() + near, rungs.end());
        rungs.push_back(rungs.front());   // closed
        std::vector<float> at(rungs.size(), 0.0f);
        for (size_t i = 1; i < rungs.size(); i++) at[i] = at[i - 1] + length(sub(rungs[i].p, rungs[i - 1].p));
        out.lap_feet = at.back();
        size_t seg = 0;
        for (float s = 0.0f; s < at.back() - spine_step * 0.5f; s += spine_step) {
            while (seg + 2 < rungs.size() && at[seg + 1] < s) seg++;
            float span = std::max(1e-6f, at[seg + 1] - at[seg]);
            float f = std::clamp((s - at[seg]) / span, 0.0f, 1.0f);
            const PathPoint& a = rungs[seg];
            const PathPoint& b = rungs[seg + 1];
            PathPoint p;
            p.p = add(a.p, mul(sub(b.p, a.p), f));
            p.lateral = normalize(add(mul(a.lateral, 1 - f), mul(b.lateral, f)));
            p.half_width = a.half_width + (b.half_width - a.half_width) * f;
            spine.push_back(p);
        }
        if (spine.size() > 4000) { report.error("The AI path is too long for the game (over 80 km)."); spine.clear(); }
        // The path's height: the road under it (the game's nearest-point searches are 3D).
        size_t off_height = 0, no_road = 0;
        float worst = 0.0f;
        Vec3 worst_at{};
        for (PathPoint& p : spine) {
            float h = cpolys.empty() ? NAN : ground_under(cpolys, p.p, 400.0f);
            if (std::isnan(h)) { no_road++; continue; }
            float dy = std::fabs(h - p.p[1]);
            if (dy > 10.0f) {
                off_height++;
                if (dy > worst) { worst = dy; worst_at = p.p; }
            }
            p.p[1] = h;
        }
        if (off_height) {
            report.warning("AIPATH_main is up to " + fmt_ft(worst) + " above or below the road (near " + fmt_pos(worst_at) +
                           ", " + std::to_string(off_height) + " of " + std::to_string(spine.size()) + " points). It was "
                           "dropped onto the road; shape the curve to follow the road's height so bridges and tunnels "
                           "pick the right level.");
        }
        if (no_road) report.warning(std::to_string(no_road) + " AI path points have no road under them at all.");
    }

    // ---- target speeds ----
    // Per point of sp, for a race along it: the backward race brakes for the same corners from the other side, and an
    // AISPEED_ marker caps the speed from it on in either direction. forward: the race the report speaks of.
    auto target_speeds = [&](const std::vector<PathPoint>& sp, bool forward) {
        size_t n = sp.size();
        std::vector<float> v(n), mph(n);
        const float grip = 32.2f * 1.1f;            // ft/s^2 of cornering the AI may use
        for (size_t i = 0; i < n; i++) {
            const Vec3& a = sp[(i + n - 3) % n].p;
            const Vec3& b = sp[i].p;
            const Vec3& c = sp[(i + 3) % n].p;
            Vec3 ab = sub(b, a), bc = sub(c, b), ac = sub(c, a);
            ab[1] = bc[1] = ac[1] = 0.0f;
            float area2 = std::fabs(ab[0] * bc[2] - ab[2] * bc[0]);
            float r = area2 > 1e-3f ? length(ab) * length(bc) * length(ac) / (2.0f * area2) : 1e9f;
            v[i] = std::min(std::sqrt(grip * r), 200.0f * 1.4667f);
        }
        // AISPEED caps: from the nearest point on, until the next marker (0 lifts the cap).
        std::vector<std::pair<size_t, float>> caps;
        for (const auto& [cap, node] : aispeeds) {
            Vec3 p = to_game(node->position());
            size_t best_i = 0;
            float best = 1e30f;
            for (size_t i = 0; i < n; i++) { float d = dist_xz(sp[i].p, p); if (d < best) { best = d; best_i = i; } }
            if (forward && best > 200.0f) {
                report.warning(base_name(node->name) + " is " + fmt_ft(best) + " from the AI path; it applies at the nearest point.");
            }
            caps.push_back({ best_i, cap });
        }
        std::sort(caps.begin(), caps.end());
        for (size_t c = 0; c < caps.size(); c++) {
            if (caps[c].second <= 0.0f) continue;
            // From this marker up to the next one (all the way round when it is the only one).
            size_t from = caps[c].first, to = caps[(c + 1) % caps.size()].first;
            size_t count = caps.size() == 1 ? n : (to + n - from) % n;
            for (size_t k = 0; k < count; k++) {
                size_t i = (from + k) % n;
                v[i] = std::min(v[i], caps[c].second * 1.4667f);
            }
        }
        // Brake ahead of slow points: v^2 <= v_next^2 + 2 * decel * step, twice around for the wrap.
        const float decel = 28.0f;
        for (int pass = 0; pass < 2; pass++) {
            for (size_t k = 0; k < n; k++) {
                size_t i = (n - 1 - k), j = (i + 1) % n;
                v[i] = std::min(v[i], std::sqrt(v[j] * v[j] + 2.0f * decel * spine_step));
            }
        }
        for (size_t i = 0; i < n; i++) mph[i] = std::clamp(v[i] / 1.4667f, 25.0f, 200.0f);
        return mph;
    };
    std::vector<float> speed_mph;
    if (!spine.empty()) {
        speed_mph = target_speeds(spine, true);
        float lo = *std::min_element(speed_mph.begin(), speed_mph.end());
        float hi = *std::max_element(speed_mph.begin(), speed_mph.end());
        char b[160];
        snprintf(b, sizeof(b), "AI path: %zu points over %s, target speeds %.0f-%.0f mph.", spine.size(),
                 fmt_ft(out.lap_feet).c_str(), lo, hi);
        report.info(b);
        if (lo < 100.0f) {
            report.warning("The AI path has corners the computer cars should take below 100 mph; on harder difficulties the "
                           "game keeps them above a minimum speed (up to 123 mph), so they may run wide there.");
        }
    }

    // ---- checkpoints on the path ----
    std::vector<int> crossing(gates.size(), -1);
    for (size_t g = 0; g < gates.size() && !spine.empty(); g++) {
        const Gate& gate = gates[g];
        int prev_side = 0;
        for (size_t i = 0; i <= spine.size(); i++) {
            const Vec3& p = spine[i % spine.size()].p;
            float d = dist_xz(p, gate.pos);
            int side = (p[0] - gate.pos[0]) * gate.dir[0] + (p[2] - gate.pos[2]) * gate.dir[2] < 0 ? -1 : 1;
            if (i > 0 && d <= gate.radius && side != prev_side) { crossing[g] = int(i % spine.size()); break; }
            prev_side = side;
        }
        if (crossing[g] < 0) {
            report.error(gate.name + " at " + fmt_pos(gate.pos) + " isn't crossed by the AI path: place it on the road "
                         "across the path, green arrow along the race direction, X scaled to half the road's width.");
        }
    }
    for (size_t g = 2; g < gates.size(); g++) {
        if (crossing[g] >= 0 && crossing[g - 1] >= 0 && crossing[g] < crossing[g - 1]) {
            report.error(gates[g].name + " comes before " + gates[g - 1].name + " along the AI path; number the checkpoints "
                         "in race order.");
        }
    }

    // ---- lanes off the road (the modder's question: must the ribbon stay on the road?) ----
    static const float lane_offsets[4] = { -0.6f, -0.2f, 0.2f, 0.6f };
    if (!spine.empty() && !cpolys.empty()) {
        size_t off_road = 0;
        Vec3 first{};
        for (size_t i = 0; i < spine.size(); i++) {
            for (float f : lane_offsets) {
                Vec3 p = add(spine[i].p, mul(spine[i].lateral, f * spine[i].half_width));
                if (std::isnan(ground_under(cpolys, p, 40.0f))) {
                    if (!off_road) first = p;
                    off_road++;
                }
            }
        }
        if (off_road) {
            report.warning(std::to_string(off_road) + " of " + std::to_string(spine.size() * 4) + " AI racing-line points "
                           "have no road under them (first near " + fmt_pos(first) + "). Keep AIPATH_main's ribbon on the "
                           "road; narrow its Extrude or move the curve.");
        }
    }

    // ---- path file (docs/rush2049_research/race.md section 3) ----
    // Written for each direction: Rush 2 races BACKWARD on its own path file (asset 0x63 + slot, docs/rush2049_research/
    // race.md section 5): the same road the other way round, the finish line first and the checkpoints in reverse.
    auto write_path = [&](const std::vector<PathPoint>& sp, const std::vector<float>& mph, const std::vector<Gate>& order,
                          std::vector<uint8_t>& d) {
        add16(d, 0);                               // base time (the game uses 90)
        for (int i = 0; i < 3; i++) add16(d, 0xFFFF); // loop / finish / arm checkpoints: recomputed at load
        add16(d, uint16_t(order.size()));
        add16(d, 0);
        for (size_t c = 0; c < 10; c++) {
            if (c < order.size()) {
                const Gate& g = order[c];
                for (float f : g.pos) addf(d, f);
                for (float f : g.dir) addf(d, f);
                add32(d, uint32_t(std::min(65535.0f, g.radius) * std::min(65535.0f, g.radius)));
                add16(d, c == 0 ? 7 : 0);          // finish: lap line, arms lap counting, loop start
                add16(d, 45);
                add16(d, 45);
            }
            else {
                for (int i = 0; i < 7; i++) add32(d, 0);
                add16(d, 0); add16(d, 0); add16(d, 0);
            }
            for (int i = 0; i < 20; i++) add16(d, 0xFFFF);
            add16(d, 0);
            addf(d, 0.0f);
        }
        add16(d, uint16_t(sp.size()));
        add16(d, 0);
        add32(d, 0);
        add8(d, 0);                                 // no branches
        add8(d, 0); add8(d, 0); add8(d, 0);
        add32(d, 0);
        add16(d, uint16_t(sp.size()));
        auto s16 = [](float f) { return uint16_t(int16_t(std::clamp(std::lround(f), -32767l, 32767l))); };
        for (const PathPoint& p : sp) for (float f : p.p) add16(d, s16(f));
        for (float f : lane_offsets) {
            add16(d, uint16_t(sp.size()));
            add8(d, 0); add8(d, 0);
            add32(d, 0);
            for (size_t i = 0; i < sp.size(); i++) {
                Vec3 p = add(sp[i].p, mul(sp[i].lateral, f * sp[i].half_width));
                float h = cpolys.empty() ? NAN : ground_under(cpolys, p, 40.0f);
                if (!std::isnan(h)) p[1] = h;
                for (float c : p) add16(d, s16(c));
                add8(d, uint8_t(std::lround(mph[i])));
                add8(d, 2);                         // steering: follow the line (stock lanes use 2 and 5)
            }
        }
    };
    if (!spine.empty() && report.ok()) {
        write_path(spine, speed_mph, gates, out.track.path);
        // Backward: from the finish line the other way round (point i is forward point n - i), lanes mirrored with
        // the direction, gates turned round, the finish first and then CHECKPOINT_n down to CHECKPOINT_1.
        size_t n = spine.size();
        std::vector<PathPoint> back(n);
        for (size_t i = 0; i < n; i++) {
            back[i] = spine[(n - i) % n];
            back[i].lateral = mul(back[i].lateral, -1.0f);
        }
        std::vector<Gate> back_gates = { gates[0] };
        for (size_t g = gates.size() - 1; g >= 1; g--) back_gates.push_back(gates[g]);
        for (Gate& g : back_gates) g.dir = mul(g.dir, -1.0f);
        write_path(back, target_speeds(back, false), back_gates, out.track.path_backward);
        for (size_t k = 0; k < 4; k++) out.track.demo_starts[0].push_back(int16_t(k * n / 4));
        for (size_t k = 0; k < 4; k++) out.track.demo_starts[1].push_back(int16_t(k * n / 4));
        out.checkpoints = int(checkpoints.size());
    }

    // ---- visual geometry ----
    std::vector<VisualTri> vtris;
    for (const auto* n : visual) append_tris(*n, vtris);
    if (vtris.empty()) report.error("Nothing to draw: the track has no visible meshes (only COL_* and markers).");
    if (!report.ok()) return false;

    // Materials -> stand-in textures (one per material).
    std::vector<int> used_materials;
    for (const VisualTri& t : vtris) if (std::find(used_materials.begin(), used_materials.end(), t.material) == used_materials.end()) used_materials.push_back(t.material);
    std::map<int, int> texture_of;      // material -> out.textures
    std::map<int, bool> cutout_of;
    for (int m : used_materials) {
        const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
        bool cutout = mat && mat->alpha_mode != "OPAQUE";
        texture_of[m] = int(out.textures.size());
        cutout_of[m] = cutout;
        out.textures.push_back(make_texture(scene, mat, cutout ? (mat->alpha_mode == "MASK" ? mat->alpha_cutoff : 0.5f) : 0.0f));
    }

    // Sections on a grid, by triangle center; a triangle too big for its cell gets a section of its own.
    std::map<std::pair<int, int>, int> cell_section;
    std::vector<Section> sections;
    for (size_t t = 0; t < vtris.size(); t++) {
        Vec3 c = mul(add(add(vtris[t].p[0], vtris[t].p[1]), vtris[t].p[2]), 1.0f / 3.0f);
        std::pair<int, int> cell = { int(std::floor(c[0] / section_size)), int(std::floor(c[2] / section_size)) };
        Vec3 origin = { (cell.first + 0.5f) * section_size, 0.0f, (cell.second + 0.5f) * section_size };
        bool fits = true;
        for (const Vec3& p : vtris[t].p) for (int k = 0; k < 3; k++) if (std::fabs(p[k] - origin[k]) > vertex_reach) fits = false;
        int s;
        if (fits) {
            auto it = cell_section.find(cell);
            if (it == cell_section.end()) {
                s = int(sections.size());
                sections.push_back(Section{});
                sections[s].origin = origin;
                cell_section[cell] = s;
            }
            else s = it->second;
        }
        else {
            s = int(sections.size());
            sections.push_back(Section{});
            sections[s].origin = c;
            for (const Vec3& p : vtris[t].p) for (int k = 0; k < 3; k++) {
                if (std::fabs(p[k] - c[k]) > vertex_reach) {
                    report.error("A visible triangle is over " + fmt_ft(vertex_reach * 2) + " across (near " + fmt_pos(c) +
                                 "); subdivide it.");
                    return false;
                }
            }
        }
        sections[s].tris.push_back(int(t));
    }
    if (sections.size() > 900) { report.error("The track is too big: over 900 sections of 312 m."); return false; }
    for (size_t s = 0; s < sections.size(); s++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "%sS%03zu", prefix.c_str(), s);
        sections[s].name = nm;
    }

    // ---- geometry container (docs/rush2049_research/geometry.md section 2) ----
    std::vector<uint8_t>& g = out.track.geometry;
    g.assign(0x28, 0);
    for (Texture& t : out.textures) {
        align8(g);
        t.offset = uint32_t(g.size());
        g.insert(g.end(), t.texels.begin(), t.texels.end());
    }
    align8(g);
    uint32_t txld_start = uint32_t(g.size());
    std::vector<uint32_t> load_lists;
    std::map<int, uint32_t> tile_of;
    std::vector<DrawMaterial> draw_materials;
    for (int m : used_materials) {
        const Texture& t = out.textures[texture_of[m]];
        const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
        draw_materials.push_back({ &t, cutout_of[m], !mat || (mat->wrap_s != 33071 && mat->wrap_t != 33071) });
        load_lists.push_back(uint32_t(g.size()));
        for (const auto& [w0, w1] : texture_load(t, mat, t.offset)) { add32(g, w0); add32(g, w1); }
        add32(g, 0xDF000000);
        add32(g, 0);
    }
    uint32_t empty_list = uint32_t(g.size());
    add32(g, 0xDF000000);
    add32(g, 0);
    uint32_t txld_end = uint32_t(g.size());

    // Names the game looks up: every section, the finish models (unchecked handles, func_8008F080) and the
    // CHKPNT / FINISH textures; sorted for func_8005BCB4's binary search.
    std::vector<std::string> names;
    for (const Section& s : sections) names.push_back(s.name);
    names.push_back(prefix + "FINISH");
    names.push_back(prefix + "FINISHB");
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return strncmp(a.c_str(), b.c_str(), 15) < 0; });
    uint32_t model_table = uint32_t(g.size());
    g.resize(g.size() + names.size() * 0x34, 0);
    uint32_t texture_table = uint32_t(g.size());
    for (const char* tn : { "CHKPNT", "FINISH" }) {
        add_name(g, tn);
        add16(g, 0); add16(g, 0);
        add32(g, 0xFFFF);
        add32(g, empty_list);
        add32(g, 0);
    }
    uint32_t palette_table = uint32_t(g.size());
    align8(g);

    for (Section& s : sections) {
        MeshList m;
        m.cmd(0xE3001001, 0x00000000);          // no TLUT: true color
        m.cmd(0xE3000F00, 0x00000000);          // no mipmaps
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);          // G_TEXTURE on, tile 0, scale 1
        emit_mesh(vtris, s.tris, used_materials, draw_materials, s.origin, 16.0f,
                  [&](MeshList& ml, size_t mi) { ml.cmd(0xDE000000, load_lists[mi]); }, m);
        s.lo = m.lo;
        s.hi = m.hi;
        s.radius = m.radius;
        m.cmd(0xE3001001, 0x00008000); // the TLUT mode Rush 2's lists leave set (RGBA16)
        m.cmd(0xDF000000, 0);
        align8(g);
        uint32_t base = uint32_t(g.size());
        g.insert(g.end(), m.vertices.begin(), m.vertices.end());
        align8(g);
        s.list = uint32_t(g.size());
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        for (const auto& [w0, w1] : m.list) { add32(g, w0); add32(g, w1); }
    }
    align8(g);
    uint32_t name_table = uint32_t(g.size());
    for (size_t i = 0; i < names.size(); i++) {
        const Section* sec = nullptr;
        for (const Section& s : sections) if (s.name == names[i]) sec = &s;
        uint32_t r = model_table + uint32_t(i) * 0x34;
        put32(g, r, 1);                              // one LOD
        put32(g, r + 4 + 8, sec ? sec->list : empty_list);
        add_name(g, names[i]);
        addf(g, sec ? sec->radius : 1.0f);
        add16(g, 0);                                 // behaviour: plain model
        add16(g, 0);
    }
    const uint32_t header[10] = { model_table, name_table, texture_table, palette_table, uint32_t(names.size()), 2, 0,
                                  txld_start, txld_end, 0 };
    for (int i = 0; i < 10; i++) put32(g, i * 4, header[i]);

    // ---- placement (docs/rush2049_research/placement.md): the sections, then the finish model ----
    std::vector<uint8_t>& pl = out.track.placement;
    add32(pl, 1);
    add32(pl, 0x18);
    add_name(pl, prefix);
    size_t records = sections.size() + 1;
    for (size_t i = 0; i < records; i++) {
        bool is_finish = i == sections.size();
        add_name(pl, is_finish ? prefix + "FINISH" : sections[i].name);
        for (int k = 0; k < 9; k++) addf(pl, (k % 4) == 0 ? 1.0f : 0.0f);
        Vec3 pos = is_finish ? (gates.empty() ? Vec3{} : gates[0].pos) : sections[i].origin;
        for (float f : pos) addf(pl, f);
        add32(pl, 0x40);
        add16(pl, uint16_t(i + 1 < records ? int16_t(i + 1) : int16_t(-1)));
        add16(pl, 0xFFFF);
        add32(pl, 0);
        for (int k = 0; k < 6; k++) addf(pl, is_finish ? 0.0f : (k < 3 ? sections[i].lo[k] : sections[i].hi[k - 3]));
    }

    // ---- tables ----
    out.track.fog[0] = settings.fog[0];
    out.track.fog[1] = settings.fog[1];
    out.track.fog[2] = settings.fog[2];
    out.track.pvs_count = 0;                         // no visibility regions: every section is drawn
    char b[200];
    size_t tri_count = vtris.size();
    snprintf(b, sizeof(b), "Track: %zu visible triangles in %zu sections, %zu textures; geometry %zu bytes.", tri_count,
             sections.size(), out.textures.size(), g.size());
    report.info(b);
    for (const auto& [name, sf] : settings.surfaces) {
        out.custom_drag[sf.material] = sf.drag;
        snprintf(b, sizeof(b), "Surface COL_%s: like %s, drag %g (slot %d).", name.c_str(), sf.like.c_str(), sf.drag, sf.material);
        report.info(b);
    }

    // ---- track select art ----
    if (extras.diorama.empty()) {
        report.info("No diorama ([menu] diorama): the track select shows Rush 2049's preview of the AI path.");
    }
    else {
        rush2::gltf::Scene d;
        std::string why;
        if (!rush2::gltf::read_glb(extras.diorama, d, why)) {
            report.warning(settings.diorama + ": " + why + " The track select shows no diorama.");
        }
        else {
            std::vector<VisualTri> dtris;
            for (const rush2::gltf::Node& n : d.nodes) append_tris(n, dtris);
            build_diorama(out.textures, d, dtris, settings.diorama, out.menu, report);
        }
    }
    return report.ok();
}
