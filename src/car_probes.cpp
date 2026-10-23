// Car lighting from light probes, the game side (include/car_probes.h, docs/graphics_effects.md#car-light-probes).
//
// - The raced track's probes are baked on a worker thread from the track's geometry and placement assets as the game
//   loads them (Rush 2's own, or a converted SF Rush / Rush 2049 / custom track in the slot) and the shared object
//   containers (0x12, 0x14), and kept on disk (track_cache blobs keyed on those bytes and the build).
// - Each race car's cube follows the probes around its body node's position, smoothed over about 0.15 s. On a Rush
//   2049 track, where the track's artists painted a lighting colour (rush2::track2049::car_light_painted) the cube
//   keeps its shape but takes that colour's brightness and tint, so the night track and the lamp pools stay as 2049
//   has them.
// - rush2::car2049::light_model draws a car's parts from per-car copies of their display lists (rush2::dl::Copier, in
//   0x80F80000-0x81000000, 64 KB per car) whose G_VTX load per-car copies of the vertices. Each draw copies the
//   vertices again (positions included: the 2049 bodies dent) with their colors times the cube at the vertex's world
//   normal. The normals are worked out once per list from its triangles (front faces are clockwise in the game's
//   left-handed space, turned the other way when the race is mirrored, as the game then swaps every list's winding),
//   summed over the vertices at the same position so a list's panels shade smoothly.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <vector>

#include "car_types.h"
#include "recomp.h"

#include "asset_catalog.h"
#include "assets.h"
#include "car_probes.h"
#include "dl_copy.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "track_cache.h"
#include "wings.h"
#include "scene_nodes.h"

namespace rush2::probes {

namespace {
    std::atomic<Mode> current_mode{ Mode::Original };

    constexpr uint32_t game_state = 0x8010C0D0;       // 0-2 menus and the car select, 3 and up a race
    constexpr uint32_t slot_records = 0x8010C258;     // 12 bytes each: u8 asset, u8 model slot, u8 mirror, ptr data
    constexpr uint32_t slot_record_size = 12;
    constexpr int slot_record_count = 64;
    constexpr uint32_t geometry_record = 0x8010C400;  // s32: the track geometry's slot record (func_800A4C98)
    constexpr int first_geometry = 0x33, first_placement = 0x3F, track_slots = 12;
    constexpr uint32_t mirrored = 0x800D0190;         // u8: the race is mirrored (the lists' winding is swapped)
    constexpr uint32_t car_body_nodes = rush2::car_types::car_states;   // + car * 0x134: the car's body scene node index
    constexpr uint32_t car_body_node_stride = 0x134;
    constexpr uint32_t scene_node_table = rush2::scene_nodes::table;
    constexpr uint32_t scene_node_size = 0x38;
    constexpr int max_scene_nodes = (int)rush2::scene_nodes::capacity;
    constexpr int max_cars = 8;
    constexpr uint32_t copies_start = 0x80F80000;
    constexpr uint32_t copies_per_car = 0x10000;

    std::mutex set_mutex;
    std::shared_ptr<const ProbeSet> probe_set;        // The raced track's, once baked
    uint64_t track_key = 0;                           // What probe_set (or the bake under way) is for
    std::atomic<uint64_t> bake_generation{ 0 };

    struct CarState {
        CarLight light;
        Cube drawn = Cube::neutral();             // light.lit() with this frame's painted colour
        std::chrono::steady_clock::time_point updated{};
    };
    CarState cars[max_cars];

    struct CarCopies {
        rush2::dl::Copier copier;
        std::map<uint64_t, uint32_t> vertices;        // (source vertices << 8 | count) -> this car's copy
    };
    std::vector<CarCopies> car_copies = [] {
        std::vector<CarCopies> v;
        for (int c = 0; c < max_cars; c++) {
            uint32_t start = copies_start + uint32_t(c) * copies_per_car;
            v.push_back({ rush2::dl::Copier(start, start + copies_per_car), {} });
        }
        return v;
    }();

    // A list's vertex loads (each range once) and their vertices' model-space normals.
    struct ListInfo {
        struct Range {
            uint32_t address, count, first_normal;
        };
        std::vector<Range> ranges;
        std::vector<std::array<float, 3>> normals;
    };
    std::unordered_map<uint32_t, ListInfo> lists;     // by the list's KSEG0 address

    uint32_t vertex_address(uint32_t w1) {
        uint32_t top = w1 >> 24;
        if (top != 0x80 && top != 0x00) return 0;
        uint32_t a = 0x80000000u | (w1 & 0xFFFFFF);
        return a >= 0x80400000 && a < 0x80B00000 ? a : 0;
    }

    uint64_t vertex_key(uint32_t address, uint32_t count) {
        return (uint64_t(address) << 8) | (count & 0xFF);
    }

    // Walks list `dl` (and the lists it calls) as the RSP runs it: the vertex loads and, at each triangle, its front
    // normal summed into its vertices' positions.
    void walk(uint8_t* rdram, uint32_t dl, ListInfo& info, std::array<uint32_t, 64>& slots,
              std::map<uint64_t, std::array<float, 3>>& sums, std::vector<std::tuple<uint32_t, uint32_t, uint64_t>>& loaded,
              float sign, int depth) {
        uint32_t src = rush2::dl::heap_list(dl);
        if (src == 0 || depth > 8) return;
        auto position = [&](uint32_t v, float out[3]) {
            for (int k = 0; k < 3; k++) out[k] = (float)(int16_t)MEM_H(k * 2, (int32_t)v);
        };
        auto pos_key = [&](uint32_t v) {
            return (uint64_t(uint16_t(MEM_H(0, (int32_t)v))) << 32) | (uint64_t(uint16_t(MEM_H(2, (int32_t)v))) << 16) |
                   uint64_t(uint16_t(MEM_H(4, (int32_t)v)));
        };
        auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
            uint32_t va = slots[(a / 2) & 63], vb = slots[(b / 2) & 63], vc = slots[(c / 2) & 63];
            if (va == 0 || vb == 0 || vc == 0) return;
            float p0[3], p1[3], p2[3];
            position(va, p0);
            position(vb, p1);
            position(vc, p2);
            float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            // Clockwise front faces: e2 x e1. Each vertex takes the face's normal weighted by the face's angle at it,
            // so a small face (a car's tail) counts as much at its corners as the big ones beside it.
            float n[3] = { e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0] };
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-6f) return;
            const float* p[3] = { p0, p1, p2 };
            uint32_t vs[3] = { va, vb, vc };
            for (int i = 0; i < 3; i++) {
                const float* a0 = p[i];
                const float* a1 = p[(i + 1) % 3];
                const float* a2 = p[(i + 2) % 3];
                float u[3] = { a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2] };
                float v[3] = { a2[0] - a0[0], a2[1] - a0[1], a2[2] - a0[2] };
                float lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), lv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (lu < 1e-6f || lv < 1e-6f) continue;
                float angle = std::acos(std::clamp((u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) / (lu * lv), -1.0f, 1.0f));
                auto& sum = sums[vs[i]];
                for (int k = 0; k < 3; k++) sum[k] += sign * n[k] / len * angle;
            }
        };
        for (uint32_t o = src, count = 0; count < 8192; o += 8, count++) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)o), w1 = (uint32_t)MEM_W(4, (int32_t)o);
            uint8_t op = w0 >> 24;
            if (op == 0x01) { // G_VTX
                uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                uint32_t a = vertex_address(w1);
                for (uint32_t i = 0; i < n; i++) slots[(end - n + i) & 63] = a ? a + i * 16 : 0;
                if (a == 0 || n == 0) continue;
                bool seen = false;
                for (const auto& r : info.ranges) seen |= r.address == a && r.count == n;
                if (!seen) {
                    info.ranges.push_back({ a, n, (uint32_t)info.normals.size() });
                    for (uint32_t i = 0; i < n; i++) {
                        info.normals.push_back({ 0, 0, 0 });
                        loaded.push_back({ (uint32_t)info.normals.size() - 1, a + i * 16, pos_key(a + i * 16) });
                    }
                }
            }
            else if (op == 0x05) { // G_TRI1
                triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF);
            }
            else if (op == 0x06 || op == 0x07) { // G_TRI2, G_QUAD
                triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF);
                triangle((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
            }
            else if (op == 0xDE || (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) { // G_DL, the 2049 conversion's call
                walk(rdram, w1, info, slots, sums, loaded, sign, depth + 1);
                if (op == 0xDE && ((w0 >> 16) & 0xFF) == 1) break;
            }
            else if (op == 0xDF) {
                break;
            }
        }
    }

    const ListInfo& list_info(uint8_t* rdram, uint32_t src) {
        auto it = lists.find(src);
        if (it != lists.end()) return it->second;
        ListInfo& info = lists[src];
        std::array<uint32_t, 64> slots{};
        std::map<uint64_t, std::array<float, 3>> sums;
        std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> loaded;
        float sign = MEM_BU(0, (int32_t)mirrored) != 0 ? -1.0f : 1.0f;
        walk(rdram, src, info, slots, sums, loaded, sign, 0);
        // Vertices at the same spot (a panel's edge drawn as two vertices for their textures) shade as one, so the light
        // turns smoothly across the car's creases (a 45 degree limit left a hard line along the beltline). Only faces
        // turned nearly opposite (the two sides of a thin fin or spoiler) keep apart, or they would cancel out.
        auto unit = [](const std::array<float, 3>& v, std::array<float, 3>& out) {
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len < 1e-6f) return false;
            for (int k = 0; k < 3; k++) out[k] = v[k] / len;
            return true;
        };
        std::map<uint64_t, std::vector<uint32_t>> at_position;
        for (const auto& [index, address, key] : loaded) {
            auto& v = at_position[key];
            if (std::find(v.begin(), v.end(), address) == v.end()) v.push_back(address);
        }
        for (const auto& [index, address, key] : loaded) {
            auto own = sums.find(address);
            std::array<float, 3> mine;
            if (own == sums.end() || !unit(own->second, mine)) continue;
            std::array<float, 3> total = own->second;
            for (uint32_t other : at_position[key]) {
                auto o = sums.find(other);
                std::array<float, 3> theirs;
                if (other == address || o == sums.end() || !unit(o->second, theirs)) continue;
                if (mine[0] * theirs[0] + mine[1] * theirs[1] + mine[2] * theirs[2] < -0.5f) continue;
                for (int k = 0; k < 3; k++) total[k] += o->second[k];
            }
            unit(total, info.normals[index]);
        }
        return info;
    }

    // The slot record of the raced track's geometry: its asset (0x33 + slot) and load address.
    bool raced_track(uint8_t* rdram, int& asset, uint32_t& address) {
        int32_t index = MEM_W(0, (int32_t)geometry_record);
        if (index < 0 || index >= slot_record_count) return false;
        uint32_t r = slot_records + uint32_t(index) * slot_record_size;
        asset = (int)((uint32_t)MEM_W(0, (int32_t)r) >> 24);
        address = (uint32_t)MEM_W(4, (int32_t)r);
        return asset >= first_geometry && asset < first_geometry + track_slots;
    }

    // Bakes (or loads) the probes of the track in geometry asset `asset`, for bake generation `generation`.
    // track49: the raced Rush 2049 track's k (its painted colours go into the probes), 0 for none.
    void bake_track(int asset, int track49, uint64_t generation) {
        std::vector<uint8_t> geometry, placement, shared[2];
        std::vector<rush2::assets::Span> spans;
        if (!rush2::assets::read_current(asset, geometry, spans) ||
            !rush2::assets::read_current(asset - first_geometry + first_placement, placement, spans)) {
            printf("[Probes] Can't read the track's geometry (asset 0x%02X)\n", asset);
            return;
        }
        rush2::assets::read_current(0x12, shared[0], spans);
        rush2::assets::read_current(0x14, shared[1], spans);
        uint64_t source = rush2::track_cache::hash(geometry.data(), geometry.size(), 0xCBF29CE484222325ull ^ bake_version);
        source = rush2::track_cache::hash(placement.data(), placement.size(), source);
        for (const auto& s : shared) source = rush2::track_cache::hash(s.data(), s.size(), source);
        PaintSource paint;
        if (track49 > 0) {
            auto rom = rush2::wings::get_rom();
            if (rom == nullptr || !rush2::track2049::car_light_paint(*rom, track49, paint.tris, paint.colours)) {
                printf("[Probes] Can't read Rush 2049 track %d's lighting colours\n", track49);
            }
            source = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(paint.tris.data()), paint.tris.size() * sizeof(float), source);
            source = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(paint.colours.data()), paint.colours.size() * sizeof(uint32_t), source);
        }
        char name[48];
        snprintf(name, sizeof(name), "car_probes_%016llx", (unsigned long long)source);

        auto set = std::make_shared<ProbeSet>();
        std::vector<uint8_t> blob;
        auto start = std::chrono::steady_clock::now();
        bool cached = rush2::track_cache::load_blob(name, source, blob) && set->deserialize(blob);
        if (!cached) {
            std::vector<rush2::catalog::Container> containers(3);
            std::string error;
            if (!rush2::catalog::read_container(geometry, containers[0], error, true)) {
                printf("[Probes] Can't read the track's models: %s\n", error.c_str());
                return;
            }
            for (int s = 0; s < 2; s++) rush2::catalog::read_container(shared[s], containers[s + 1], error, true);
            std::vector<const rush2::catalog::Container*> refs = { &containers[0], &containers[1], &containers[2] };
            set = bake(refs, rush2::catalog::read_placement(placement), nullptr, {}, paint.colours.empty() ? nullptr : &paint);
            rush2::track_cache::save_blob(name, source, set->serialize());
        }
        printf("[Probes] Track asset 0x%02X: %zu probes %s in %.2fs\n", asset, set->count(), cached ? "loaded" : "baked",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        fflush(stdout);
        std::lock_guard lock{ set_mutex };
        if (bake_generation == generation) probe_set = std::move(set);
    }

    float luminance(const float c[3]) {
        return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    }
}

void set_mode(Mode mode) {
    current_mode = mode;
}

Mode mode() {
    return current_mode;
}

void reset() {
    for (auto& c : car_copies) {
        c.copier.reset();
        c.vertices.clear();
    }
    lists.clear();
    for (auto& c : cars) c.light.valid = false;
}

bool update(uint8_t* rdram) {
    if (current_mode != Mode::Probes || MEM_W(0, (int32_t)game_state) < 3) {
        return false;
    }
    int asset;
    uint32_t address;
    if (!raced_track(rdram, asset, address)) {
        return false;
    }
    int track49 = rush2::track2049::car_light_track();
    uint64_t key = (uint64_t(asset) << 56) ^ (uint64_t(track49) << 48) ^ (uint64_t(address) << 24) ^ rush2::assets::generation();
    std::shared_ptr<const ProbeSet> set;
    {
        std::lock_guard lock{ set_mutex };
        if (key != track_key) {
            track_key = key;
            probe_set.reset();
            uint64_t generation = ++bake_generation;
            std::thread(bake_track, asset, track49, generation).detach();
        }
        set = probe_set;
    }
    if (!set) {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    for (int car = 0; car < max_cars; car++) {
        int node = (int32_t)MEM_W(0, (int32_t)(car_body_nodes + uint32_t(car) * car_body_node_stride));
        if (node < 0 || node >= max_scene_nodes) continue;
        uint32_t transform = (uint32_t)MEM_W(4, (int32_t)(scene_node_table + uint32_t(node) * scene_node_size));
        if (transform < 0x80000000u || transform >= 0x80800000u) continue;
        float pos[3];
        for (int k = 0; k < 3; k++) {
            uint32_t bits = (uint32_t)MEM_W(0x24 + k * 4, (int32_t)transform);
            memcpy(&pos[k], &bits, 4);
        }
        // A 2049 track's painted colour under the car (CarLight::Painted).
        uint32_t rgba;
        CarLight::Painted painted{ {}, rush2::track2049::car_light_painted(car) };
        bool has_paint = painted.amount > 0 && rush2::track2049::car_light(car, rgba);
        for (int k = 0; k < 3 && has_paint; k++) painted.rgb[k] = float((rgba >> (24 - k * 8)) & 0xFF) / 255.0f;
        static const CarLight::Params params;
        // Test aid: RUSH2_PROBE_TEST=1 logs car 0's probes every second; =cube also lights every car with a fixed
        // cube (above white, +x green, the rest dark gray), which shows whether the normals face the world the right
        // way (the roof lit, the tail and underside dark, the side facing +x, the left one, green).
        static const char* test = getenv("RUSH2_PROBE_TEST");
        Cube target;
        if (test) {
            target = CarLight::target(*set, pos, params.radius);
            static auto logged = now;
            if (car == 0 && now - logged > std::chrono::seconds(1)) {
                logged = now;
                printf("[Probes] car 0 at %.1f %.1f %.1f: +x %.2f -x %.2f up %.2f down %.2f +z %.2f -z %.2f\n", pos[0], pos[1],
                       pos[2], luminance(target.c[0]), luminance(target.c[1]), luminance(target.c[2]),
                       luminance(target.c[3]), luminance(target.c[4]), luminance(target.c[5]));
                fflush(stdout);
            }
            // =path records every car's position and painted colour (amount, r g b) 20 times a second, for
            // tools/car_probes_test.cpp --path.
            static std::chrono::steady_clock::time_point path_at[max_cars];
            static const auto path_start = now;
            // =frames logs car 1's position at every update (each view of each frame).
            if (strcmp(test, "frames") == 0 && car == 1) {
                printf("[ProbeFrame] %.4f %.2f %.2f %.2f\n", std::chrono::duration<float>(now - path_start).count(), pos[0],
                       pos[1], pos[2]);
                fflush(stdout);
            }
            if (strcmp(test, "path") == 0 && now - path_at[car] > std::chrono::milliseconds(50)) {
                path_at[car] = now;
                printf("[ProbePath] %.3f %d %.2f %.2f %.2f %.2f %.3f %.3f %.3f\n",
                       std::chrono::duration<float>(now - path_start).count(), car, pos[0], pos[1], pos[2],
                       has_paint ? painted.amount : 0.0f, painted.rgb[0], painted.rgb[1], painted.rgb[2]);
                fflush(stdout);
            }
            if (strcmp(test, "cube") == 0) {
                static const float faces[6][3] = { { 0.2f, 1.5f, 0.2f }, { 0.2f, 0.2f, 0.2f }, { 1, 1, 1 },
                                                   { 0.2f, 0.2f, 0.2f }, { 0.2f, 0.2f, 0.2f }, { 0.2f, 0.2f, 0.2f } };
                memcpy(target.c, faces, sizeof(faces));
            }
        }
        CarState& state = cars[car];
        float dt = state.light.valid ? std::chrono::duration<float>(now - state.updated).count() : 0.0f;
        if (test && strcmp(test, "cube") == 0) {
            state.light.valid = true;
            state.drawn = target;
        }
        else {
            state.light.step(*set, pos, has_paint ? &painted : nullptr, dt, params);
            state.drawn = state.light.lit(state.light.painted.amount > 0 ? &state.light.painted : nullptr, params);
        }
        state.updated = now;
    }
    return true;
}

uint32_t relight(uint8_t* rdram, int car, uint32_t dl, const float rot[9]) {
    uint32_t src = rush2::dl::heap_list(dl);
    if (car < 0 || car >= max_cars || src == 0 || !cars[car].light.valid) {
        return 0;
    }
    const ListInfo& info = list_info(rdram, src);
    CarCopies& copies = car_copies[car];
    uint32_t copy = copies.copier.copy(rdram, dl, [&](uint32_t& w0, uint32_t& w1) {
        if ((w0 >> 24) != 0x01) return;
        uint32_t a = vertex_address(w1), n = (w0 >> 12) & 0xFF;
        if (a == 0 || n == 0) return;
        uint64_t key = vertex_key(a, n);
        auto it = copies.vertices.find(key);
        uint32_t to = it != copies.vertices.end() ? it->second : 0;
        if (to == 0) {
            to = copies.copier.alloc(n * 16);
            if (to == 0) return;
            copies.vertices[key] = to;
        }
        w1 = (w1 & 0xFF000000) | (to & 0xFFFFFF);
    });
    if (copy == 0) {
        return 0;
    }
    const Cube& cube = cars[car].drawn;
    for (const auto& r : info.ranges) {
        auto it = copies.vertices.find(vertex_key(r.address, r.count));
        if (it == copies.vertices.end()) continue;
        for (uint32_t i = 0; i < r.count; i++) {
            int32_t from = (int32_t)(r.address + i * 16), to = (int32_t)(it->second + i * 16);
            for (int w = 0; w < 3; w++) MEM_W(w * 4, to) = MEM_W(w * 4, from);
            uint32_t colour = (uint32_t)MEM_W(12, from);
            const auto& n = info.normals[r.first_normal + i];
            if (n[0] != 0 || n[1] != 0 || n[2] != 0) {
                // The normal in the world: n * rot (rows), then the cube's light on it.
                float wn[3];
                for (int k = 0; k < 3; k++) wn[k] = n[0] * rot[k] + n[1] * rot[3 + k] + n[2] * rot[6 + k];
                float len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                if (len > 1e-6f) {
                    for (float& v : wn) v /= len;
                    float light[3];
                    cube.eval(wn, light);
                    uint32_t lit = colour & 0xFF;
                    for (int k = 0; k < 3; k++) {
                        float c = float((colour >> (24 - k * 8)) & 0xFF) * light[k];
                        lit |= uint32_t(std::clamp(std::lround(c), 0l, 255l)) << (24 - k * 8);
                    }
                    colour = lit;
                }
            }
            MEM_W(12, to) = (int32_t)colour;
        }
    }
    return copy;
}

}
