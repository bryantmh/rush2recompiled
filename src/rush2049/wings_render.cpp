// Draws Rush 2049's wings and flames on Rush 2's cars.
//
// Model: Rush 2049's file 77 holds the six wing objects (WINGSWING1L/R..3L/R) and six flame objects
// (WINGSFLAME1L/R..3L/R) as prebuilt F3DEX2 display lists. The decompressed file is copied as is into spare RDRAM
// and its pointers are rebased the way Rush 2049's loader does (func_80096CBC): G_VTX, G_DL and G_SETTIMG addresses
// in object display lists are relative to the file, and G_SETTIMG addresses in the texture load lists (TXLD chunk)
// are relative to the texture data (IMAG chunk). File layout: word 0 is the offset of a directory of 12-byte
// {tag, offset, size or count} entries; OBHD holds 0x58-byte object headers {name[12], flags, radius, u16,
// s16 lod count, {lod flags, distance, display list, vertices}[4]}.
//
// Drawing: Rush 2 draws a car body by emitting a G_DL to the body model's display list under the body's matrix
// (func_8007AA48, hook rush2_model_draw). For a car whose wings are out, that display list is swapped for one in
// a side buffer that calls the body and then, for each side, pushes the wing's car-local matrix, draws the wing,
// pushes the flame's matrix under it, draws the flame, and pops both. The game's display list keeps its size.
//
// Rush 2049 builds the same transforms (func_800924F4, func_80091874): wing = rotation about its X axis by the
// tilt, translated by (slide, height, 0) in world units; flame = scale along Y, translated (0, 0.55, 0) under the
// wing. Both games use 1/16 world unit vertices, and Rush 2's node matrices are in vertex units, so translations
// are scaled by 16. The wings are drawn unlit with the render state Rush 2049 sets up for every model.

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "recomp.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "wings_internal.h"

namespace {
    // Spare RDRAM above the game's heap, below 16MB so display list addresses fit in 24 bits. The game heap ends at
    // 0x80B00000 (src/assets.cpp), interpolation uses 0x80B00000-0x80C00000 and the Controls screen's glyphs
    // 0x80C00000-0x80C90000.
    constexpr uint32_t side_start = 0x80D00000;
    constexpr uint32_t side_end = 0x80E00000;
    constexpr uint32_t model_address = 0x80E00000;
    constexpr uint32_t model_max_size = 0x10000;
    uint32_t side_cursor = side_start;

    // Game addresses.
    constexpr uint32_t node_pool = 0x800D9E90;
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t car_body_node = 0x80219DD0; // Body node index of car i at + i * 0x134 (moved from 0x80113F90, src/rush2049/car2049.cpp).
    constexpr uint32_t car_body_node_stride = 0x134;
    constexpr uint32_t lighting_cache = 0x800E7DE1; // func_8007AA48: nonzero while G_LIGHTING is on.
    constexpr uint32_t palette_cache = 0x80111954; // func_80078190: palette last loaded into TMEM, 0 for none.
    constexpr uint32_t car_palette_records = 0x802217E0; // func_8008582C: car i's palette record (0x18 bytes: name, first, last index, +0x14 data).
    constexpr uint32_t car_palette_stride = 0x18;

    constexpr uint32_t G_DL_CALL = 0xDE000000;
    constexpr uint32_t G_ENDDL_W0 = 0xDF000000;
    constexpr uint32_t G_MTX_PUSH_MUL_MODELVIEW = 0xDA380000;
    constexpr uint32_t G_POPMTX_W0 = 0xD8380002;
    constexpr uint32_t G_MTX_MODELVIEW_EX = 0;

    // Render state Rush 2049 has in place when it draws a model and that these display lists rely on rather than
    // set: 2-cycle mode, perspective correct bilinear texturing, Z buffer, smooth shading and fog with no lighting
    // or texture generation, and texturing on.
    constexpr uint32_t prologue[][2] = {
        { 0xE7000000, 0x00000000 }, // G_RDPPIPESYNC
        { 0xE3000A01, 0x00100000 }, // CYCLETYPE = 2CYCLE
        { 0xE3000C00, 0x00080000 }, // TEXTPERSP = PERSP
        { 0xE3001201, 0x00002000 }, // TEXTFILT = BILERP
        { 0xD9F9FFFF, 0x00000000 }, // clear LIGHTING | TEXTURE_GEN
        { 0xD9FFFFFF, 0x00210005 }, // set ZBUFFER | SHADE | SHADING_SMOOTH | FOG
        { 0xD7000002, 0xFFFFFFFF }, // G_TEXTURE on, scale 1
    };
    // Undoes the state the wing lists change that Rush 2's car lists rely on: texture LOD back to tile, TLUT back
    // to RGBA16, RGB dither and G_TEXTURE back to what Rush 2's init lists set (dither off, one level).
    constexpr uint32_t epilogue[][2] = {
        { 0xE7000000, 0x00000000 },
        { 0xE3000F00, 0x00000000 }, // TEXTLOD = TILE
        { 0xE3001001, 0x00008000 }, // TEXTLUT = RGBA16
        { 0xE3001801, 0x000000C0 }, // RGBDITHER = DISABLE
        { 0xD7000002, 0xFFFFFFFF }, // G_TEXTURE on, tile 0, no mip levels, scale 1
    };

    std::mutex model_mutex;
    std::vector<uint8_t> model_file; // Decompressed file 77, or empty.
    std::atomic<uint32_t> model_generation = 1;
    uint32_t loaded_generation = 0;
    bool model_loaded = false;

    // Display lists of the 12 objects in Rush 2049's handle order: style * 4 + {wing L, wing R, flame L, flame R}.
    std::array<uint32_t, 12> object_dl{};

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }

    struct Chunk {
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct ModelLayout {
        Chunk imag, txld, obhd, objs;
        uint32_t dir = 0;
    };

    bool parse_layout(const std::vector<uint8_t>& d, ModelLayout& out) {
        if (d.size() < 16) {
            return false;
        }
        out.dir = be32(d, 0);
        for (size_t o = out.dir; o + 12 <= d.size(); o += 12) {
            Chunk c{ be32(d, o + 4), be32(d, o + 8) };
            if (memcmp(&d[o], "IMAG", 4) == 0) out.imag = c;
            else if (memcmp(&d[o], "TXLD", 4) == 0) out.txld = c;
            else if (memcmp(&d[o], "OBHD", 4) == 0) out.obhd = c;
            else if (memcmp(&d[o], "OBJS", 4) == 0) out.objs = c;
        }
        return out.imag.size && out.txld.size && out.obhd.size && out.objs.size;
    }

    // Rebases one display list (and the lists it calls) in place. fixed holds the word offsets already rebased.
    bool rebase_dl(uint8_t* rdram, const ModelLayout& layout, uint32_t file_size, uint32_t start,
                   std::vector<bool>& fixed, int depth) {
        if (depth > 8) {
            return false;
        }
        const uint32_t base = model_address & 0x1FFFFFFF;
        bool in_txld = start >= layout.txld.offset && start < layout.txld.offset + layout.txld.size;
        for (uint32_t o = start; o + 8 <= file_size; o += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(model_address + o));
            uint32_t w1 = (uint32_t)MEM_W(0, (int32_t)(model_address + o + 4));
            uint32_t op = w0 >> 24;
            uint32_t rel = w1 & 0xFFFFFF;
            uint32_t target = UINT32_MAX;
            if (op == 0x01 || op == 0xDE) {
                target = rel;
            }
            else if (op == 0xFD) {
                target = in_txld ? rel + layout.imag.offset : rel;
            }
            if (target != UINT32_MAX && !fixed[(o + 4) / 4]) {
                if (target >= file_size) {
                    return false;
                }
                fixed[(o + 4) / 4] = true;
                MEM_W(0, (int32_t)(model_address + o + 4)) = (int32_t)(((target + base) & 0xFFFFFF) | (w1 & 0x0F000000));
                if (op == 0xDE && !rebase_dl(rdram, layout, file_size, rel, fixed, depth + 1)) {
                    return false;
                }
            }
            if (op == 0xDF || (op == 0xDE && ((w0 >> 16) & 0xFF) != 0)) {
                return true;
            }
        }
        return false;
    }

    // Copies the model into RDRAM and rebases it. Runs on the game thread.
    void load_model(uint8_t* rdram) {
        std::vector<uint8_t> file;
        {
            std::lock_guard lock{ model_mutex };
            file = model_file;
        }
        model_loaded = false;

        ModelLayout layout;
        if (file.empty() || file.size() > model_max_size || file.size() % 4 != 0 || !parse_layout(file, layout)) {
            return;
        }
        for (size_t i = 0; i < file.size(); i += 4) {
            MEM_W(0, (int32_t)(model_address + i)) = (int32_t)be32(file, i);
        }

        std::vector<bool> fixed(file.size() / 4, false);
        uint32_t file_size = uint32_t(file.size());

        // Objects by name, mapped to handle order.
        for (uint32_t i = 0; i < layout.obhd.size; i++) {
            uint32_t rec = layout.obhd.offset + i * 0x58;
            if (rec + 0x58 > file_size) {
                return;
            }
            char name[13] = {};
            memcpy(name, &file[rec], 12);
            int style, side, kind;
            char lr;
            if (sscanf(name, "WINGSWING%d%c", &style, &lr) == 2) kind = 0;
            else if (sscanf(name, "WINGSFLAME%d%c", &style, &lr) == 2) kind = 2;
            else continue;
            side = lr == 'R' ? 1 : 0;
            if (style < 1 || style > 3) {
                continue;
            }
            uint32_t dl = be32(file, rec + 0x18 + 8); // LOD 0 display list.
            if (!rebase_dl(rdram, layout, file_size, dl, fixed, 0)) {
                printf("[Wings] Bad display list in %s\n", name);
                return;
            }
            object_dl[(style - 1) * 4 + kind + side] = model_address + dl;
        }

        // The texture load lists, called from the object lists, run back to back through the TXLD chunk.
        for (uint32_t o = layout.txld.offset; o < layout.txld.offset + layout.txld.size; o += 8) {
            uint32_t op = (uint32_t)MEM_W(0, (int32_t)(model_address + o)) >> 24;
            uint32_t w1 = (uint32_t)MEM_W(0, (int32_t)(model_address + o + 4));
            if (op == 0xFD && !fixed[(o + 4) / 4]) {
                fixed[(o + 4) / 4] = true;
                uint32_t target = (w1 & 0xFFFFFF) + layout.imag.offset;
                MEM_W(0, (int32_t)(model_address + o + 4)) = (int32_t)(((target + (model_address & 0x1FFFFFFF)) & 0xFFFFFF) | (w1 & 0x0F000000));
            }
        }

        for (uint32_t dl : object_dl) {
            if (dl == 0) {
                printf("[Wings] Wing model is missing objects\n");
                return;
            }
        }
        model_loaded = true;
    }

    // N64 fixed point matrix (s15.16, row vectors): integer halves then fractions.
    void write_mtx(uint8_t* rdram, uint32_t addr, const float m[4][4]) {
        int32_t fixed[16];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                fixed[r * 4 + c] = (int32_t)std::lround(m[r][c] * 65536.0f);
            }
        }
        for (int i = 0; i < 8; i++) {
            uint32_t a = (uint32_t)fixed[i * 2], b = (uint32_t)fixed[i * 2 + 1];
            MEM_W(0, (int32_t)(addr + i * 4)) = (int32_t)((a & 0xFFFF0000) | (b >> 16));
            MEM_W(0, (int32_t)(addr + 0x20 + i * 4)) = (int32_t)((a << 16) | (b & 0xFFFF));
        }
    }

    uint32_t side_alloc(uint32_t bytes) {
        bytes = (bytes + 63) & ~63u;
        if (side_cursor + bytes > side_end) {
            side_cursor = side_start;
        }
        uint32_t addr = side_cursor;
        side_cursor += bytes;
        return addr;
    }

    struct DlWriter {
        uint8_t* rdram;
        uint32_t addr;
        uint32_t count = 0;
        void cmd(uint32_t w0, uint32_t w1) {
            MEM_W(0, (int32_t)(addr + count * 8)) = (int32_t)w0;
            MEM_W(0, (int32_t)(addr + count * 8 + 4)) = (int32_t)w1;
            count++;
        }
        void cmd(const GfxCommand& c) {
            cmd(c.values.word0, c.values.word1);
        }
        void group(uint32_t id) {
            GfxCommand c[2];
            gEXMatrixGroup(c, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, G_MTX_MODELVIEW_EX,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
            cmd(c[0]);
            cmd(c[1]);
        }
        void pop_group() {
            GfxCommand c;
            gEXPopMatrixGroup(&c, G_MTX_MODELVIEW_EX);
            cmd(c);
        }
    };

    // Wing IDs: 0100 vvgg gggg gggg gggg gggg pppp pppp (p = car * 4 + part). Distinct from interpolation.cpp's.
    uint32_t wing_id(uint32_t view, uint32_t gen, uint32_t car, uint32_t part) {
        return 0x40000000u | ((view & 3) << 26) | ((gen & 0x3FFFF) << 8) | ((car * 4 + part) & 0xFF);
    }

    int car_for_node(uint8_t* rdram, uint32_t node) {
        if (node < node_pool) {
            return -1;
        }
        uint32_t index = (node - node_pool) / node_size;
        for (int car = 0; car < rush2::wings::max_cars; car++) {
            if ((uint32_t)MEM_W(0, (int32_t)(car_body_node + car * car_body_node_stride)) == index) {
                return car;
            }
        }
        return -1;
    }
}

void rush2::wings::on_rom_changed() {
    std::vector<uint8_t> file;
    auto rom = get_rom();
    // File 77: WINGSWING1L..3R, WINGSFLAME1L..3R.
    if (rom != nullptr && !rom->read_file(77, file)) {
        file.clear();
    }
    {
        std::lock_guard lock{ model_mutex };
        model_file = std::move(file);
    }
    model_generation++;
    reload_sound();
}

extern "C" void rush2_wings_model_draw(uint8_t* rdram, recomp_context* ctx) {
    rush2::wings::draw_car_body(rdram, ctx);
}

// func_8007AA48, before the G_DL to a model's display list ($fp) is written. $s7 = scene graph node.
void rush2::wings::draw_car_body(uint8_t* rdram, recomp_context* ctx) {
    uint32_t generation = model_generation.load();
    if (generation != loaded_generation) {
        loaded_generation = generation;
        load_model(rdram);
    }
    if (!model_loaded || !enabled()) {
        return;
    }

    int car = car_for_node(rdram, (uint32_t)ctx->r23);
    if (car < 0) {
        return;
    }
    Pose pose;
    if (!get_pose(rdram, car, pose)) {
        return;
    }

    uint32_t view = 0, gen = 0;
    rush2_interp_get_generation(&view, &gen);

    // Wing matrices: tilt about X, then the slide out to the side. Flames: stretched along Y, 0.55 above the wing.
    float c = std::cos(pose.tilt), s = std::sin(pose.tilt);
    if (std::fabs(pose.tilt) <= 1e-4f) {
        c = 1.0f;
        s = 0.0f;
    }
    uint32_t mtx = side_alloc(4 * 0x40);
    for (int side = 0; side < 2; side++) {
        float sign = side ? 1.0f : -1.0f;
        float wing[4][4] = {
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, c, -s, 0.0f },
            { 0.0f, s, c, 0.0f },
            { sign * pose.slide * 16.0f, pose.height * 16.0f, 0.0f, 1.0f },
        };
        float flame[4][4] = {
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, pose.flame_scale[side], 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.55f * 16.0f, 0.0f, 1.0f },
        };
        write_mtx(rdram, mtx + side * 0x80, wing);
        write_mtx(rdram, mtx + side * 0x80 + 0x40, flame);
    }

    constexpr uint32_t max_cmds = 80;
    DlWriter dl{ rdram, side_alloc(max_cmds * 8) };
    dl.cmd(G_DL_CALL, (uint32_t)ctx->r30); // The car body.
    for (const auto& c : prologue) {
        dl.cmd(c[0], c[1]);
    }
    for (int side = 0; side < 2; side++) {
        uint32_t base = pose.style * 4;
        dl.group(wing_id(view, gen, car, side));
        dl.cmd(G_MTX_PUSH_MUL_MODELVIEW, mtx + side * 0x80);
        dl.cmd(G_DL_CALL, object_dl[base + side]);
        if (pose.flames) {
            dl.group(wing_id(view, gen, car, 2 + side));
            dl.cmd(G_MTX_PUSH_MUL_MODELVIEW, mtx + side * 0x80 + 0x40);
            dl.cmd(G_DL_CALL, object_dl[base + 2 + side]);
            dl.cmd(G_POPMTX_W0, 0x40);
            dl.pop_group();
        }
        dl.cmd(G_POPMTX_W0, 0x40);
        dl.pop_group();
    }
    for (const auto& c : epilogue) {
        dl.cmd(c[0], c[1]);
    }
    // func_8007AA48 only emits G_LIGHTING changes when a model's lighting differs from what it last set, so put
    // lighting back the way it left it.
    if (MEM_BU(0, (int32_t)lighting_cache) != 0) {
        dl.cmd(0xD9FFFFFF, 0x00020000);
    }
    // The wing lists load their own palettes over the car's in TMEM. Only three of a car's nodes carry its palette
    // (node +0x2C, set by func_8008582C for the nodes at car record +0x2, +0x6 and +0x32); the body, the front
    // panels and the rest draw with whatever TLUT is loaded, so after the wings they would take the wing colors.
    // Reload the car's palette the way func_80078190 does: SETTIMG, SETTILE 7 at TMEM 0x100 + first index,
    // LOADTLUT of (last - first) entries. If the TLUT in place is not the car's, at least make the next node with a
    // palette reload it (func_80078190 skips the load when its data pointer is the one it loaded last).
    uint32_t loaded = (uint32_t)MEM_W(0, (int32_t)palette_cache);
    uint32_t car_palette = car_palette_records + car * car_palette_stride;
    if (loaded != 0 && (uint32_t)MEM_W(0, (int32_t)(car_palette + 0x14)) == loaded) {
        uint32_t first = MEM_BU(0, (int32_t)(car_palette + 0x10));
        uint32_t last = MEM_BU(0, (int32_t)(car_palette + 0x11));
        dl.cmd(0xFD100000, loaded);
        dl.cmd(0xE8000000, 0);
        dl.cmd(0xF5000000 | ((first + 0x100) & 0x1FF), 0x07000000);
        dl.cmd(0xE6000000, 0);
        dl.cmd(0xF0000000, 0x07000000 | (((last - first) & 0x3FF) << 14));
        dl.cmd(0xE7000000, 0);
    }
    else {
        MEM_W(0, (int32_t)palette_cache) = 0;
    }
    // The car's paint: the primitive and environment colors func_8007AA48 set from the body node (+0x30, +0x34),
    // which the car's other parts draw with. The flames change both.
    uint32_t node = (uint32_t)ctx->r23;
    uint32_t node_flags = (uint32_t)MEM_W(0, (int32_t)node);
    auto node_color = [&](uint32_t offset) {
        return (uint32_t(MEM_BU(0, (int32_t)(node + offset))) << 24) | (uint32_t(MEM_BU(0, (int32_t)(node + offset + 1))) << 16) |
            (uint32_t(MEM_BU(0, (int32_t)(node + offset + 2))) << 8) | MEM_BU(0, (int32_t)(node + offset + 3));
    };
    if (node_flags & 0x2000) {
        dl.cmd(0xFA000000, node_color(0x30));
    }
    if (node_flags & 0x4000) {
        dl.cmd(0xFB000000, node_color(0x34));
    }
    dl.cmd(G_ENDDL_W0, 0);

    ctx->r30 = (int32_t)dl.addr;
}
