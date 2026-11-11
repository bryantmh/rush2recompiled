// Fill Scenery Gaps, the game side (include/scenery_fill.h, docs/graphics_effects.md#fill-scenery-gaps).
//
// Which draws are the track: race_load_track_800A5110 makes a scene node for every placement record with
// placement_walk_80081790 (0x800A5704), so the nodes made between its call and its return (the scene node count at
// 0x800FAE58 before and after) are the track's models: sections, props, signs. The sky model and the cars are made
// elsewhere.
//
// Two-sided walls: each of those nodes' model lists is called between gEXSetTwoSidedWalls(1) and (0) (RT64 draws
// their near-vertical culled triangles a second time wound the other way, lib/patches/rt64.patch, RSP::drawIndexedTri),
// and every other model's between gEXSetTwoSidedWalls(2) and (0): its faces count as something seen from behind, so no
// copy goes on a plane where anything else already shows from that side (a sign's modeled back, a two-sided board,
// an arrow layered over a board).
//
// Extended ground: the raced track's underlay (src/scenery_fill_build.cpp) is built on a worker thread from the
// track's geometry and placement assets and the shared object containers as the game loaded them (the same inputs as
// the car light probes, src/car_probes.cpp), kept on disk (track_cache blobs keyed on those bytes and the build), and
// written into RDRAM above 16 MB (two arenas from recomp::alloc, used in turn so a new track's never overwrites the one
// being drawn; RT64 reads them with gEXSetRDRAMExtended): per texture a load list (its texels in the track's own
// format, so texture packs replace it as they do the track's), per chunk its vertices (s16 in 1/16 ft from the
// chunk's center) and a list of G_VTX / G_TRI2 by strip. In each view the underlay goes in right before the first
// track model drawn (after the sky models, which draw first and without depth, before the rest of the world): the
// chunks in front of the camera and within the view's far plane, each under a float matrix of its own (its center
// less the camera's, times the view's root modelview, loaded as the whole modelview as src/rush2049/battle_render.cpp
// does for its models) in a matrix group of its own so frame interpolation moves it with the camera. The draws use
// their own render state between pushes and pops of the geometry mode, other modes and combiner, with fog and the
// depth buffer like the track and no culling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "asset_catalog.h"
#include "assets.h"
#include "car_probes.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "scene_nodes.h"
#include "scenery_fill.h"
#include "track1.h"
#include "track2049.h"
#include "track_cache.h"
#include "track_mod_runtime.h"

extern "C" uint32_t rush2_interp_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count);
extern "C" bool rush2_interp_view_matrices(uint32_t view, uint32_t* root, uint32_t* projection, uint32_t* lookat);
extern "C" uint32_t rush2_interp_view_generation(uint32_t view);
extern "C" void rush2_interp_get_generation(uint32_t* view, uint32_t* gen);

namespace {
    using rush2::scenery_fill::Mesh;

    constexpr uint32_t game_state = 0x8010C0D0;
    constexpr uint32_t cameras = 0x800E79D0;          // Per view, 0x40: +0x18 forward, +0x24 position
    constexpr uint32_t views = 0x802401F0;            // Player views, 0x54 each: +0x38 far plane (f32, feet)
    constexpr uint32_t view_size = 0x54;
    constexpr uint32_t palette_cache = 0x80111954;    // func_80078190: palette last loaded into TMEM, 0 for none
    constexpr int first_placement = 0x3F;
    constexpr uint32_t arena_size = 0x400000;         // 4 MB per arena; Los Angeles, the biggest, needs about 2.2
    constexpr uint32_t ring_size = 0x200000;          // The views' display lists and matrices, reused in turn
    constexpr int max_views = 4;

    constexpr uint32_t G_DL_CALL = 0xDE000000;
    constexpr uint32_t G_ENDDL_W0 = 0xDF000000;
    constexpr uint32_t G_POPMTX_W0 = 0xD8380002;
    constexpr uint32_t G_RDPPIPESYNC = 0xE7000000;
    constexpr uint32_t G_MTX_MODELVIEW_EX = 0;

    std::atomic<bool> on_flag{ false };
    int placement_first = 0, placement_end = 0;     // The track's scene nodes [first, end)

    struct ChunkRef {
        float center[3];
        float radius;
        uint32_t dl;
    };
    struct OneSided {
        uint32_t address;                            // In RDRAM
        uint8_t bytes[8];                            // Its first texels, as the file has them
    };
    struct Placed {
        std::vector<ChunkRef> chunks;
        std::vector<OneSided> one_sided;            // The road blocks' texels (gEXSetTwoSidedSkip)
    };

    std::mutex mutex;                                // Guards ready and key (held only to swap them)
    std::shared_ptr<const Placed> ready;
    uint64_t key = 0;
    std::atomic<uint64_t> generation{ 0 };
    std::mutex arena_mutex[2];                       // Held by a worker while it writes that arena
    uint32_t arenas[2] = {};
    int next_arena = 0;
    uint32_t ring = 0, ring_cursor = 0;
    uint8_t* rdram_base = nullptr;
    bool drawn[max_views] = {};
    uint32_t one_sided_sent = 0;                     // The road blocks model_draw last found loaded (for the log)

    bool in_race(uint8_t* rdram) {
        switch (MEM_W(0, (int32_t)game_state)) {
            case 3:
            case 6:
            case 9:
            case 10:
                return true;
            default:
                return false;
        }
    }

    uint32_t rdram_address(uint8_t* rdram, void* p) {
        return (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
    }

    float read_f32(uint8_t* rdram, uint32_t address) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)address);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    // An N64 fixed-point matrix (s15.16, rows): integer halves, then fractions.
    void read_mtx(uint8_t* rdram, uint32_t addr, float m[4][4]) {
        addr = 0x80000000u | (addr & 0x00FFFFFF);
        for (int i = 0; i < 8; i++) {
            uint32_t hi = (uint32_t)MEM_W(0, (int32_t)(addr + i * 4));
            uint32_t lo = (uint32_t)MEM_W(0, (int32_t)(addr + 0x20 + i * 4));
            m[(i * 2) / 4][(i * 2) % 4] = (int32_t)((hi & 0xFFFF0000) | (lo >> 16)) / 65536.0f;
            m[(i * 2 + 1) / 4][(i * 2 + 1) % 4] = (int32_t)((hi << 16) | (lo & 0xFFFF)) / 65536.0f;
        }
    }

    struct Writer {
        uint8_t* rdram;
        uint32_t at, end;
        bool full = false;
        uint32_t align(uint32_t a) {
            at = (at + a - 1) & ~(a - 1);
            return at;
        }
        void word(uint32_t w) {
            if (at + 4 > end) {
                full = true;
                return;
            }
            MEM_W(0, (int32_t)at) = (int32_t)w;
            at += 4;
        }
        void cmd(uint32_t w0, uint32_t w1) {
            word(w0);
            word(w1);
        }
        void cmd(const GfxCommand& c) { cmd(c.values.word0, c.values.word1); }
    };

    // Writes the underlay into an arena: the textures' load lists, then each chunk's vertices and list.
    bool encode(uint8_t* rdram, const Mesh& mesh, uint32_t arena, Placed& out, uint64_t gen) {
        Writer w{ rdram, arena, arena + arena_size };
        std::vector<uint32_t> load_dl(mesh.textures.size());
        for (size_t i = 0; i < mesh.textures.size(); i++) {
            const auto& t = mesh.textures[i];
            uint32_t texels = w.align(8);
            for (size_t b = 0; b + 3 < t.texels.size() + 3; b += 4) {
                uint32_t v = 0;
                for (int k = 0; k < 4; k++) v = (v << 8) | (b + k < t.texels.size() ? t.texels[b + k] : 0);
                w.word(v);
            }
            uint32_t palette = w.align(8);
            for (size_t p = 0; p < t.palette.size(); p += 2) {
                w.word((uint32_t(t.palette[p]) << 16) | (p + 1 < t.palette.size() ? t.palette[p + 1] : 0));
            }
            load_dl[i] = w.align(8);
            const uint32_t row_bytes = t.width * (4u << t.siz) / 8;
            const uint32_t words = std::max(1u, row_bytes / 8);
            const uint32_t load_siz = 2;   // G_IM_SIZ_16b: 4 and 8-bit textures load as 16-bit texels
            const uint32_t load_count = t.siz == 0 ? (t.width * t.height + 3) >> 2 : t.siz == 1 ? (t.width * t.height + 1) >> 1 : t.width * t.height;
            const uint32_t dxt = (2048 + words - 1) / words;
            auto log2 = [](uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; };
            w.cmd(0xE3001001, t.tlut);                                          // G_SETOTHERMODE_H TEXTLUT
            if (!t.palette.empty()) {
                w.cmd(0xFD100000, palette);                                     // SETTIMG RGBA 16b
                w.cmd(0xE8000000, 0);                                           // TILESYNC
                w.cmd(0xF5000100, 0x07000000);                                  // SETTILE 7 at TMEM 0x100
                w.cmd(0xE6000000, 0);                                           // LOADSYNC
                w.cmd(0xF0000000, 0x07000000 | (((uint32_t)t.palette.size() - 1) & 0x3FF) << 14);   // LOADTLUT
                w.cmd(G_RDPPIPESYNC, 0);
            }
            w.cmd(0xFD000000 | (uint32_t(t.fmt) << 21) | (load_siz << 19), texels);         // SETTIMG
            w.cmd(0xF5000000 | (uint32_t(t.fmt) << 21) | (load_siz << 19), 0x07000000);      // SETTILE 7 (load)
            w.cmd(0xE6000000, 0);
            w.cmd(0xF3000000, 0x07000000 | ((load_count - 1) & 0xFFF) << 12 | (dxt & 0xFFF));   // LOADBLOCK
            w.cmd(G_RDPPIPESYNC, 0);
            // SETTILE 0 (render): wrap in both directions, masked to the size.
            w.cmd(0xF5000000 | (uint32_t(t.fmt) << 21) | (uint32_t(t.siz) << 19) | ((row_bytes / 8) << 9),
                  (log2(t.height) << 14) | (log2(t.width) << 4));
            w.cmd(0xF2000000, ((t.width - 1) << 2) << 12 | ((t.height - 1) << 2));            // SETTILESIZE 0
            w.cmd(G_ENDDL_W0, 0);
        }
        for (const auto& c : mesh.chunks) {
            if (generation.load() != gen) return false;
            // Vertices, strip by strip, then the list that draws them.
            std::vector<uint32_t> vtx(c.strips.size());
            for (size_t s = 0; s < c.strips.size(); s++) {
                vtx[s] = w.align(8);
                const auto& strip = c.strips[s];
                for (const auto* row : { &strip.top, &strip.bottom }) {
                    for (const auto& v : *row) {
                        int32_t p[3], st[2];
                        for (int k = 0; k < 3; k++) p[k] = std::clamp((int32_t)std::lround((v.pos[k] - c.center[k]) * 16.0f), -32768, 32767);
                        for (int k = 0; k < 2; k++) st[k] = std::clamp((int32_t)std::lround(v.st[k] * 32.0f), -32768, 32767);
                        w.word((uint32_t(p[0]) << 16) | (uint32_t(p[1]) & 0xFFFF));
                        w.word(uint32_t(p[2]) << 16);
                        w.word((uint32_t(st[0]) << 16) | (uint32_t(st[1]) & 0xFFFF));
                        w.word((uint32_t(v.rgb[0]) << 24) | (uint32_t(v.rgb[1]) << 16) | (uint32_t(v.rgb[2]) << 8) | 0xFF);
                    }
                }
            }
            uint32_t dl = w.align(8);
            int texture = -1;
            for (size_t s = 0; s < c.strips.size(); s++) {
                const auto& strip = c.strips[s];
                if (strip.texture != texture) {
                    texture = strip.texture;
                    w.cmd(G_DL_CALL, load_dl[size_t(texture)]);
                }
                uint32_t n = (uint32_t)strip.top.size();      // Vertices per row; bottom row from n
                w.cmd(0x01000000 | ((n * 2) << 12) | ((n * 2) << 1), vtx[s]);   // G_VTX: 2n vertices from slot 0
                for (uint32_t i = 0; i + 1 < n; i++) {
                    uint32_t a = i, b = i + 1, cc = n + i + 1, d = n + i;
                    w.cmd(0x06000000 | (a * 2) << 16 | (b * 2) << 8 | (cc * 2), (a * 2) << 16 | (cc * 2) << 8 | (d * 2));   // G_TRI2
                }
            }
            w.cmd(G_ENDDL_W0, 0);
            if (w.full) break;
            ChunkRef ref;
            memcpy(ref.center, c.center, sizeof(ref.center));
            ref.radius = c.radius;
            ref.dl = dl;
            out.chunks.push_back(ref);
        }
        if (w.full) printf("[Fill] The underlay doesn't fit its arena; %zu of %zu chunks drawn\n", out.chunks.size(), mesh.chunks.size());
        return true;
    }

    // The raced track version's diorama table key (rush2::scenery_fill::find_fit), or "" for none: an SF Rush track,
    // a custom track by its folder (the SF Rush arcade and PlayStation versions), else a stock Rush 2 one by its slot.
    std::string guide_key(uint8_t* rdram, int asset) {
        static const char* const slots[] = { "VEGAS", "NYONE", "HAWAII", "NYTWO", "ALCATRAZ", "LA", "SEATTLE", "HALFPIPE",
                                             "CRASH", "PIPE", "ATARI", "STUNT1" };
        if (int k = rush2::track1::race_track(); k >= 1) return "sfrush:" + std::to_string(k);
        if (std::string folder = rush2::track_mod::race_folder(); !folder.empty()) return folder;
        int slot = asset - 0x33;
        if (rush2::track2049::converted_race(rdram) || slot < 0 || slot >= 12) return "";
        return std::string("rush2:") + slots[slot];
    }

    // The track's diorama laid over it, or false: the diorama containers (Rush 2's asset 3, SF Rush's asset 10
    // converted) read once and kept.
    bool make_guide(const std::string& key, rush2::scenery_fill::Guide& guide) {
        const rush2::scenery_fill::Fit* fit = rush2::scenery_fill::find_fit(key);
        if (!fit) return false;
        static std::mutex containers_mutex;
        static std::unique_ptr<rush2::catalog::Container> containers[2];
        std::lock_guard lock{ containers_mutex };
        int d = rush2::scenery_fill::sfrush_diorama(*fit) ? 1 : 0;
        if (!containers[d]) {
            std::vector<uint8_t> data;
            std::string error;
            bool ok = false;
            if (d == 0) {
                std::vector<rush2::assets::Span> spans;
                ok = rush2::assets::read_current(3, data, spans);
            }
            else if (auto rom = rush2::track1::get_rom()) {
                ok = rush2::track1::convert_container(*rom, 10, data, error);
            }
            auto c = std::make_unique<rush2::catalog::Container>();
            if (!ok || !rush2::catalog::read_container(data, *c, error, true)) {
                printf("[Fill] Can't read the dioramas (%s)\n", error.c_str());
                return false;
            }
            containers[d] = std::move(c);
        }
        return rush2::scenery_fill::make_guide(*containers[d], *fit, guide);
    }

    // Builds (or loads) the underlay of the track in geometry asset `asset` (loaded at `address`) and writes it into
    // `arena`. key: its diorama's (guide_key).
    void build_track(int asset, uint32_t address, std::string key, uint64_t gen, int arena_index) {
        std::vector<uint8_t> geometry, placement, shared[2];
        std::vector<rush2::assets::Span> spans;
        if (!rush2::assets::read_current(asset, geometry, spans) ||
            !rush2::assets::read_current(asset - 0x33 + first_placement, placement, spans)) {
            printf("[Fill] Can't read the track's geometry (asset 0x%02X)\n", asset);
            return;
        }
        rush2::assets::read_current(0x12, shared[0], spans);
        rush2::assets::read_current(0x14, shared[1], spans);
        uint64_t source = rush2::track_cache::hash(geometry.data(), geometry.size(), 0xCBF29CE484222325ull ^ (0x46494C4Cull << 32) ^ rush2::scenery_fill::build_version);
        source = rush2::track_cache::hash(placement.data(), placement.size(), source);
        for (const auto& s : shared) source = rush2::track_cache::hash(s.data(), s.size(), source);
        rush2::scenery_fill::Guide guide;
        bool guided = !key.empty() && make_guide(key, guide);
        if (guided) {
            uint64_t h = guide.hash();
            source = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(&h), sizeof(h), source);
        }
        char name[48];
        snprintf(name, sizeof(name), "scenery_fill_%016llx", (unsigned long long)source);

        auto start = std::chrono::steady_clock::now();
        Mesh mesh;
        std::vector<uint8_t> blob;
        bool cached = rush2::track_cache::load_blob(name, source, blob) && mesh.deserialize(blob);
        if (!cached) {
            std::vector<rush2::catalog::Container> containers(3);
            std::string error;
            if (!rush2::catalog::read_container(geometry, containers[0], error, true)) {
                printf("[Fill] Can't read the track's models: %s\n", error.c_str());
                return;
            }
            for (int s = 0; s < 2; s++) rush2::catalog::read_container(shared[s], containers[s + 1], error, true);
            std::vector<const rush2::catalog::Container*> refs = { &containers[0], &containers[1], &containers[2] };
            mesh = rush2::scenery_fill::build(refs, rush2::catalog::read_placement(placement), guided ? &guide : nullptr);
            rush2::track_cache::save_blob(name, source, mesh.serialize());
        }
        if (generation.load() != gen) return;
        auto placed = std::make_shared<Placed>();
        {
            std::lock_guard arena_lock{ arena_mutex[arena_index] };
            if (!encode(rdram_base, mesh, arenas[arena_index], *placed, gen)) return;
        }
        // The road blocks' texels where the game loads the geometry, and their first bytes there (model_draw checks
        // them: the worker can run before the game has finished loading the track).
        for (uint32_t offset : mesh.one_sided) {
            if (offset + sizeof(OneSided::bytes) > geometry.size()) continue;
            OneSided o;
            o.address = address + offset;
            memcpy(o.bytes, &geometry[offset], sizeof(o.bytes));
            placed->one_sided.push_back(o);
        }
        printf("[Fill] Track asset 0x%02X: %zu chunks %s in %.2fs, diorama %s, %zu one-sided textures\n", asset,
               placed->chunks.size(), cached ? "loaded" : "built",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), guided ? guide.key.c_str() : "none",
               placed->one_sided.size());
        fflush(stdout);
        std::lock_guard lock{ mutex };
        if (generation.load() == gen) ready = std::move(placed);
    }

    // The view's underlay display list: the chunks it can see, each under its own matrix. 0 when none.
    uint32_t underlay_list(uint8_t* rdram, uint32_t view, const Placed& placed) {
        uint32_t root = 0, projection = 0, lookat = 0;
        if (!rush2_interp_view_matrices(view, &root, &projection, &lookat)) return 0;
        float root_m[4][4];
        read_mtx(rdram, root, root_m);
        float cam[3], fwd[3];
        for (int k = 0; k < 3; k++) {
            cam[k] = read_f32(rdram, cameras + view * 0x40 + 0x24 + k * 4);
            fwd[k] = read_f32(rdram, cameras + view * 0x40 + 0x18 + k * 4);
        }
        float fwd_len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        float far_ft = read_f32(rdram, views + view * view_size + 0x38) * std::max(rush2::draw_distance(), 1.0f);
        if (!(far_ft > 0) || far_ft > 1e6f) far_ft = 1e6f;
        const uint32_t gen = rush2_interp_view_generation(view);

        // Room for the worst case: every chunk visible.
        const uint32_t bytes = 512 + uint32_t(placed.chunks.size()) * (7 * 8 + 64);
        if (bytes > ring_size) return 0;
        if (ring_cursor + bytes > ring + ring_size) ring_cursor = ring;
        Writer w{ rdram, ring_cursor, ring_cursor + bytes };
        // Matrices first, then the list.
        std::vector<std::pair<uint32_t, uint32_t>> drawn_chunks;   // chunk index, matrix
        for (size_t i = 0; i < placed.chunks.size(); i++) {
            const ChunkRef& c = placed.chunks[i];
            float rel[3] = { c.center[0] - cam[0], c.center[1] - cam[1], c.center[2] - cam[2] };
            float dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
            if (dist - c.radius > far_ft) continue;
            if (fwd_len > 0 && (rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2]) / fwd_len < -c.radius) continue;
            // translate(rel * 16) times the root modelview (rows).
            float m[4][4];
            for (int r = 0; r < 3; r++) {
                for (int col = 0; col < 4; col++) m[r][col] = root_m[r][col];
            }
            for (int col = 0; col < 4; col++) {
                m[3][col] = rel[0] * 16.0f * root_m[0][col] + rel[1] * 16.0f * root_m[1][col] + rel[2] * 16.0f * root_m[2][col] + root_m[3][col];
            }
            uint32_t addr = w.align(8);
            for (int k = 0; k < 16; k++) {
                uint32_t bits;
                memcpy(&bits, &m[k / 4][k % 4], 4);
                w.word(bits);
            }
            drawn_chunks.emplace_back((uint32_t)i, addr);
        }
        if (drawn_chunks.empty()) return 0;
        uint32_t list = w.align(8);
        GfxCommand c[2];
        gEXPushGeometryMode(&c[0]);
        w.cmd(c[0]);
        gEXPushOtherMode(&c[0]);
        w.cmd(c[0]);
        gEXPushCombineMode(&c[0]);
        w.cmd(c[0]);
        w.cmd(G_RDPPIPESYNC, 0);
        w.cmd(0xD9F9F9FF, 0x00210005);   // clear CULL_BOTH | LIGHTING | TEXTURE_GEN; set ZBUFFER | SHADE | SHADING_SMOOTH | FOG
        w.cmd(0xE3000A01, 0x00100000);   // CYCLETYPE = 2CYCLE
        w.cmd(0xE3000C00, 0x00080000);   // TEXTPERSP = PERSP
        w.cmd(0xE3000D01, 0x00000000);   // TEXTDETAIL = CLAMP
        w.cmd(0xE3000F00, 0x00000000);   // TEXTLOD = TILE
        w.cmd(0xE3001402, 0x00000C00);   // TEXTCONV = FILT
        w.cmd(0xE3001201, 0x00002000);   // TEXTFILT = BILERP
        w.cmd(0xE200001C, 0xC8112078);   // render mode FOG_SHADE_A, AA_ZB_OPA_SURF2
        w.cmd(0xE2001E01, 0x00000000);   // ALPHACOMPARE = NONE
        w.cmd(0xE2001D00, 0x00000000);   // ZSRC = PIXEL
        // (TEXEL0 - 0) * SHADE + 0, alpha 1; then the combined color.
        w.cmd(0xFC000000 | (1u << 20) | (4u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u,
              (15u << 28) | (7u << 15) | (7u << 12) | (6u << 9) | (15u << 24) | (7u << 21) | (7u << 18) | (0u << 6) | (7u << 3) | 0u);
        w.cmd(0xD7000002, 0xFFFFFFFF);   // G_TEXTURE on, tile 0, scale 1
        for (const auto& [index, matrix] : drawn_chunks) {
            uint32_t id = 0x70000000u | ((view & 3) << 26) | ((gen & 0x3FFF) << 12) | (index & 0xFFF);
            gEXMatrixGroup(c, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, G_MTX_MODELVIEW_EX,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
            w.cmd(c[0]);
            w.cmd(c[1]);
            gEXMatrixFloat(c, matrix, 0x02);   // push, load (the push bit is stored inverted)
            w.cmd(c[0]);
            w.cmd(c[1]);
            w.cmd(G_DL_CALL, placed.chunks[index].dl);
            w.cmd(G_POPMTX_W0, 0x40);
            gEXPopMatrixGroup(&c[0], G_MTX_MODELVIEW_EX);
            w.cmd(c[0]);
        }
        w.cmd(G_RDPPIPESYNC, 0);
        gEXPopCombineMode(&c[0]);
        w.cmd(c[0]);
        gEXPopOtherMode(&c[0]);
        w.cmd(c[0]);
        gEXPopGeometryMode(&c[0]);
        w.cmd(c[0]);
        w.cmd(G_ENDDL_W0, 0);
        if (w.full) return 0;
        ring_cursor = w.at;
        // The underlay's palettes went into TMEM over the game's.
        MEM_W(0, (int32_t)palette_cache) = 0;
        return list;
    }
}

void rush2::scenery_fill::set_enabled(bool on) {
    on_flag.store(on, std::memory_order_relaxed);
}

bool rush2::scenery_fill::enabled() {
    return on_flag.load(std::memory_order_relaxed);
}

void rush2::scenery_fill::view_begin(uint8_t* rdram, uint32_t view) {
    if (view < max_views) drawn[view] = false;
    if (!enabled() || !in_race(rdram)) return;
    int asset;
    uint32_t address;
    if (!rush2::probes::raced_track(rdram, asset, address)) return;
    uint64_t k = (uint64_t(asset) << 56) ^ (uint64_t(address) << 24) ^ rush2::assets::generation();
    std::lock_guard lock{ mutex };
    if (k == key) return;
    key = k;
    ready.reset();
    if (arenas[0] == 0) {
        rdram_base = rdram;
        for (uint32_t& a : arenas) a = (rdram_address(rdram, recomp::alloc(rdram, arena_size + 16)) + 15) & ~15u;
        ring = (rdram_address(rdram, recomp::alloc(rdram, ring_size + 16)) + 15) & ~15u;
        ring_cursor = ring;
    }
    uint64_t gen = ++generation;
    int arena_index = next_arena;
    next_arena ^= 1;
    std::thread(build_track, asset, address, guide_key(rdram, asset), gen, arena_index).detach();
}

void rush2::scenery_fill::model_draw(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled() || !in_race(rdram)) return;
    const uint32_t node = (uint32_t)ctx->r23;
    if (node < rush2::scene_nodes::table || node >= rush2::scene_nodes::table_end) return;
    const int index = int((node - rush2::scene_nodes::table) / rush2::scene_nodes::node_size);
    constexpr uint32_t max_one_sided = 8;
    GfxCommand cmds[6 + max_one_sided];
    uint32_t count = 0;
    if (index < placement_first || index >= placement_end) {
        // Any other model (cars, props placed by code) only counts as something seen from behind on a wall's plane,
        // so no copy goes over it.
        gEXSetTwoSidedWalls(&cmds[count++], 2);
        cmds[count].values.word0 = G_DL_CALL;
        cmds[count++].values.word1 = (uint32_t)ctx->r30;
        gEXSetTwoSidedWalls(&cmds[count++], 0);
        ctx->r30 = (uint64_t)(int64_t)(int32_t)rush2_interp_side_dl(rdram, cmds, count);
        return;
    }

    uint32_t view = 0, gen = 0;
    rush2_interp_get_generation(&view, &gen);
    if (view < max_views && !drawn[view]) {
        drawn[view] = true;
        std::shared_ptr<const Placed> placed;
        {
            std::lock_guard lock{ mutex };
            placed = ready;
        }
        // The road blocks the two-sided walls leave out (RT64 empties the list at each frame's start).
        if (placed) {
            uint32_t sent = 0;
            for (const OneSided& o : placed->one_sided) {
                bool loaded = sent < max_one_sided;
                // A signed offset: an unsigned one makes MEM_BU's address unsigned, reading far past RDRAM.
                for (int32_t i = 0; loaded && i < (int32_t)sizeof(o.bytes); i++) loaded = MEM_BU(i, (int32_t)o.address) == o.bytes[i];
                if (loaded) {
                    gEXSetTwoSidedSkip(&cmds[count++], o.address);
                    sent++;
                }
            }
            if (sent != one_sided_sent) {
                one_sided_sent = sent;
                printf("[Fill] Two-sided walls leave out %u of %zu one-sided textures\n", sent, placed->one_sided.size());
                fflush(stdout);
            }
        }
        uint32_t list = placed ? underlay_list(rdram, view, *placed) : 0;
        if (list != 0) {
            gEXSetRDRAMExtended(&cmds[count++], 1);
            cmds[count].values.word0 = G_DL_CALL;
            cmds[count++].values.word1 = list;
            gEXSetRDRAMExtended(&cmds[count++], 0);
        }
    }
    gEXSetTwoSidedWalls(&cmds[count++], 1);
    cmds[count].values.word0 = G_DL_CALL;
    cmds[count++].values.word1 = (uint32_t)ctx->r30;
    gEXSetTwoSidedWalls(&cmds[count++], 0);
    ctx->r30 = (uint64_t)(int64_t)(int32_t)rush2_interp_side_dl(rdram, cmds, count);
}

// race_load_track_800A5110 at 0x800A5704 (the placement walk's call) and 0x800A570C (after it returns).
extern "C" void rush2_scenery_fill_walk_begin(uint8_t* rdram, recomp_context* ctx) {
    placement_first = MEM_W(0, (int32_t)rush2::scene_nodes::count);
    placement_end = placement_first;
}

extern "C" void rush2_scenery_fill_walk_end(uint8_t* rdram, recomp_context* ctx) {
    placement_end = MEM_W(0, (int32_t)rush2::scene_nodes::count);
}
