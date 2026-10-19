// Shared by the Rush 2049 arcade converters (src/arcade2049/): the arcade data reader (arcade_data.cpp, a port of
// tools/arcade2049/arcade.py), a glTF binary writer (arcade_glb.cpp, of tools/stock_track_glb.py's Glb and
// tools/car_mod.py's write_glb) and small vector helpers. Formats: docs/arcade2049.md.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json/json.hpp"

#include "arcade2049.h"

namespace rush2::arcade2049 {
    using V3 = std::array<double, 3>;
    using V2 = std::array<double, 2>;

    inline V3 operator+(const V3& a, const V3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    inline V3 operator-(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    inline V3 operator*(const V3& a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
    inline double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    inline V3 cross(const V3& a, const V3& b) {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }
    inline double norm(const V3& a) { return std::sqrt(dot(a, a)); }
    // a / |a|, or a unchanged when it has no length (numpy's `a / (norm or 1)`).
    inline V3 unit(const V3& a) { double n = norm(a); return n > 0 ? a * (1.0 / n) : a; }

    // A 3x3 matrix, row major; points are row vectors (p M), as the arcade's placements are.
    using M3 = std::array<double, 9>;
    inline V3 mul(const V3& p, const M3& m) {
        return { p[0] * m[0] + p[1] * m[3] + p[2] * m[6], p[0] * m[1] + p[1] * m[4] + p[2] * m[7],
                 p[0] * m[2] + p[1] * m[5] + p[2] * m[8] };
    }

    constexpr double feet_per_meter = 3.2808399;
    // Game feet (x negated against glTF) -> glTF meters.
    inline V3 to_gltf(const V3& p) { return { -p[0] / feet_per_meter, p[1] / feet_per_meter, p[2] / feet_per_meter }; }

    // ---------------------------------------------------------------------------------------------- arcade data
    constexpr int fmt_yiq422 = 1, fmt_ayiq8422 = 9, fmt_rgb565 = 10;

    // A texture descriptor (0x50 bytes in objects.rom; arcade.py Texture).
    struct Texture {
        std::string name;
        int fmt = 0;
        int w = 0, h = 0;
        uint32_t offset = 0, texture_mode = 0;
        std::array<int, 16> y{};
        std::array<std::array<int, 3>, 4> i{}, q{};
        bool clamp_s() const { return (texture_mode & 0x40) != 0; }
        bool clamp_t() const { return (texture_mode & 0x80) != 0; }
        // Level 0 as w*h RGBA (row 0 first); false for a format the reader doesn't know or data that ends early.
        bool rgba(const std::vector<uint8_t>& texels, std::vector<uint8_t>& out) const;
        // Voodoo s/t run 0-256 across the long side and 256 x aspect across the short one.
        V2 uv_scale() const {
            double l = std::max(w, h);
            return { 256.0 * w / l, 256.0 * h / l };
        }
    };

    struct Vertex {
        V3 pos;
        V2 st;
        std::array<uint8_t, 4> rgba;
    };

    struct Lod {
        std::vector<Vertex> verts;
        std::vector<std::array<uint16_t, 4>> tris;   // v0, v1, v2, texture index
    };

    struct Object {
        std::string name;
        Lod lod0;                                    // the only LOD the converters use
    };

    struct WorldEntry {
        std::string name;
        M3 m;
        V3 pos;
    };

    struct DobsEntry {
        std::string name;
        std::vector<std::string> script;
        M3 m;
        V3 pos;
    };

    struct CollisionPoly {
        uint32_t flags = 0;
        std::vector<V3> pts;                         // world
        V3 normal;                                   // world
        int seg = -1;                                // the spline segment (flags & 0x3000), else -1
    };

    struct PathRecord {
        V3 pos;                                      // world
        double speed;                                // ft/s
        int kind;                                    // the steering behaviour
    };

    struct PathTable {
        uint32_t lap_count = 0;
        std::vector<PathRecord> records;
    };

    // One folder of arcade data: a track (TRACKS/TRACKn) or a car's color scheme (CARS/CARn/COLn). Each reader
    // returns false (with `error`) on a file that is missing or ends early.
    class Track {
    public:
        bool open(const std::filesystem::path& folder, std::string& error);
        const std::filesystem::path& folder() const { return dir; }
        std::vector<Object> objects;
        std::vector<Texture> textures;
        std::map<std::string, size_t> by_name;       // the last object of a name, as a dict built in order
        const std::vector<uint8_t>& texels();

        const Object* find(const std::string& name) const;
        // The object a placement names (arcade.py object_named): worlds.rom keeps 8 characters of a name and dobs.rom's
        // names can be longer than the object table's 15; a flipbook's frames are <name>_1..n (the first is returned).
        const Object* object_named(const std::string& name) const;

        bool worlds(std::vector<WorldEntry>& out, std::string& error) const;
        bool dobs(std::vector<DobsEntry>& out, std::string& error) const;
        bool collision(std::vector<CollisionPoly>& out, std::string& error) const;
        // roaddat.rom's spline segments as Rush 2's 33-float SEG records (arcade.py segments).
        bool segments(std::vector<std::array<double, 33>>& out, std::string& error) const;
        bool path_tables(std::vector<PathTable>& out, std::string& error) const;
        bool read(const std::string& name, std::vector<uint8_t>& out) const;

    private:
        std::filesystem::path dir;
        std::vector<uint8_t> texel_data;
        bool texels_read = false;
    };

    // The dpath / roaddat frame (z, x, -y of world) and dobs.rom placements (x, z, y) to world.
    inline V3 to_world_path(const V3& p) { return { p[1], -p[2], p[0] }; }
    inline V3 to_world_dobs(const V3& p) { return { p[0], p[2], p[1] }; }

    // PNG bytes of w x h RGBA.
    std::vector<uint8_t> png(const std::vector<uint8_t>& rgba, int w, int h);

    // ---------------------------------------------------------------------------------------------- glb writer
    // A minimal glTF 2.0 binary writer: meshes, materials with embedded PNGs, Empties and node animations.
    class Glb {
    public:
        explicit Glb(const std::string& generator, bool default_material = true);
        nlohmann::json js;

        struct Prim {
            int material = 0;
            std::vector<V3> verts, normals;          // glTF space
            std::vector<V2> uvs;
            std::vector<std::array<double, 4>> colors;   // empty: none
            std::vector<std::array<uint32_t, 3>> tris;
        };
        struct Trs {
            V3 translation{};
            std::array<double, 4> rotation{ 0, 0, 0, 1 };
            V3 scale{ 1, 1, 1 };
        };

        int image(const std::string& name, const std::vector<uint8_t>& png_bytes);
        // wrap: glTF sampler (wrapS, wrapT). Returns the material's index.
        int material(const std::string& name, std::optional<int> image, const std::string& alpha_mode, bool double_sided,
                     std::array<int, 2> wrap);
        int textured_mesh(const std::string& name, const std::vector<Prim>& prims, const std::optional<Trs>& trs = {});
        // A mesh with the default material and zero uvs; attrs: custom float attributes, one value per vertex.
        int mesh(const std::string& name, const std::vector<V3>& verts, const std::vector<V3>& normals,
                 const std::vector<std::array<uint32_t, 3>>& tris,
                 const std::vector<std::pair<std::string, std::vector<double>>>& attrs = {});
        int empty(const std::string& name, const std::optional<std::array<double, 16>>& matrix = {});
        void animate(const std::string& name, int node, const std::vector<double>& times,
                     const std::vector<std::array<double, 4>>* rotation, const std::vector<V3>* scale);
        std::vector<uint8_t> bytes();

    private:
        std::vector<uint8_t> bin;
        int view(const void* data, size_t size, int target);
        int accessor_f(const std::vector<float>& values, const char* kind, int width, int target, bool minmax);
        int accessor_u(const std::vector<uint32_t>& values, int target);
        int node(nlohmann::json n, const std::optional<Trs>& trs);
    };

    // An Empty at a checkpoint (game pos, game direction, radius ft): its -Z along the direction, Y up, X scaled to
    // the half width in meters. Column major (stock_track_glb.py gate_matrix).
    std::array<double, 16> gate_matrix(const V3& pos, const V3& direction, double radius);

    inline std::vector<uint8_t> text_bytes(const std::string& s) { return { s.begin(), s.end() }; }

    // Python's round(): halves to even (the default floating point rounding mode).
    inline double round_even(double v) { return std::nearbyint(v); }
}
