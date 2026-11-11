// Draws Rush 2049's battle models on any track (include/battle_render.h).
//
// Models: Rush 2049's files 76 (WEPICON_* pickups, WEP_* weapons, WPR_* projectiles, WFX_* effects), 63 (the HUD's
// BCOIN_* coins), 61 (the explosion's 30 frames NEXPLOSIONG1-30, EXP_*) and 62 (COIN_GLOWG1, the glow around the
// race tracks' coins, src/collectibles.cpp) hold prebuilt F3DEX2 display lists. Each
// file is copied as it is into spare RDRAM and its pointers are rebased the way Rush 2049's loader does, as
// src/rush2049/wings_render.cpp does for the wings: G_VTX, G_DL and G_SETTIMG addresses in object display lists are relative to
// the file, G_SETTIMG addresses in the texture load lists (TXLD chunk) to the texture data (IMAG chunk). An object
// header (OBHD chunk, 0x58 bytes) is {name[16], f32 radius, u16 kind, s16 lods, {u16, u16, f32 distance, u32 display
// list, u32 vertices}[4]}; the first level of detail is drawn.
//
// Drawing: at the end of each view (func_8007C624's exit, before the frame interpolation's own hook there) a display
// list is added that draws every placed model under a matrix of its own: the model's axes and its position less the
// camera's times 16 (vertices are 1/16 world units in both games), multiplied onto the view's root modelview as the
// game's own node matrices are. A model that rides with the view's camera is drawn in view space instead: its
// matrix times the view's rotation as the whole modelview, under the projection without the rotation (the game keeps
// the rotation in the projection matrix; src/interpolation.cpp, rush2_interp_node_matrix, has the reason). The
// models are drawn unlit with the render state Rush 2049 has for every model, in the primitive color given.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "arrows.h"
#include "battle_render.h"
#include "rush2049_rom.h"
#include "rush2_hooks.h"
#include "wings.h"

extern "C" void rush2_interp_get_generation(uint32_t* view, uint32_t* gen);
extern "C" bool rush2_interp_view_matrices(uint32_t view, uint32_t* root, uint32_t* projection, uint32_t* lookat);

namespace {
    // Spare RDRAM (src/rush2049/wings_render.cpp lists the ranges below these): the model files, then the display lists.
    struct ModelFile {
        int file;
        uint32_t address, max_size;
    };
    constexpr ModelFile model_files[] = {
        { 76, 0x80E20000, 0x24000 },
        { 63, 0x80E44000, 0x4000 },
        { 61, 0x80E48000, 0x38000 },
        { 62, 0x80F00000, 0x18000 },   // 0x16C70 bytes; 0x80F00000-0x81000000 is otherwise unused
    };
    // Images decoded for rush2::battle_render::image, in the room after file 63 (13736 bytes).
    constexpr int image_file = 63;
    constexpr uint32_t image_start = 0x80E44000 + 0x3800, image_end = 0x80E44000 + 0x4000;
    constexpr uint32_t side_start = 0x80E80000;
    constexpr uint32_t side_end = 0x80F00000;
    uint32_t side_cursor = side_start;

    constexpr uint32_t cameras = 0x800E79D0;         // Per view, 0x40: +0x24 position
    constexpr uint32_t game_state = 0x8010C0D0;      // 0-2 the menus, track select and car select; 3 and up a race
    constexpr uint32_t main_dl_head = 0x80022FD8;
    constexpr uint32_t lighting_cache = 0x800E7DE1;  // func_8007AA48: nonzero while G_LIGHTING is on.
    constexpr uint32_t palette_cache = 0x80111954;   // func_80078190: palette last loaded into TMEM, 0 for none.

    constexpr uint32_t G_DL_CALL = 0xDE000000;
    constexpr uint32_t G_ENDDL_W0 = 0xDF000000;
    constexpr uint32_t G_POPMTX_W0 = 0xD8380002;
    constexpr uint32_t G_MTX_LOAD_MODELVIEW_W0 = 0xDA380003;
    constexpr uint32_t G_MTX_LOAD_PROJECTION_W0 = 0xDA380007;
    constexpr uint32_t G_MTX_MUL_PROJECTION_W0 = 0xDA380005;
    constexpr uint32_t G_MTX_MODELVIEW_EX = 0, G_MTX_PROJECTION_EX = 1;
    constexpr float teleport_distance = 32.0f;

    // The render state Rush 2049 has in place when it draws a model, and what Rush 2 expects afterwards
    // (src/rush2049/wings_render.cpp).
    constexpr uint32_t prologue[][2] = {
        { 0xE7000000, 0x00000000 }, // G_RDPPIPESYNC
        { 0xE3000A01, 0x00100000 }, // CYCLETYPE = 2CYCLE
        { 0xE3000C00, 0x00080000 }, // TEXTPERSP = PERSP
        { 0xE3001201, 0x00002000 }, // TEXTFILT = BILERP
        { 0xD9F9FFFF, 0x00000000 }, // clear LIGHTING | TEXTURE_GEN
        { 0xD9FFFFFF, 0x00210005 }, // set ZBUFFER | SHADE | SHADING_SMOOTH | FOG
        { 0xD7000002, 0xFFFFFFFF }, // G_TEXTURE on, scale 1
    };
    constexpr uint32_t epilogue[][2] = {
        { 0xE7000000, 0x00000000 },
        { 0xE3000F00, 0x00000000 }, // TEXTLOD = TILE
        { 0xE3001001, 0x00008000 }, // TEXTLUT = RGBA16
        { 0xE3001801, 0x000000C0 }, // RGBDITHER = DISABLE
        { 0xD7000002, 0xFFFFFFFF }, // G_TEXTURE on, tile 0, no mip levels, scale 1
    };

    struct Item {
        bool on = false;
        uint32_t dl = 0;
        float m[9] = {}, pos[3] = {};
        uint32_t rgba = 0xFFFFFFFF;
        int view = -1;
        int hidden_view = -1;   // a view it isn't drawn in (hide_in_view), -1 none
        bool attached = false;
        bool billboard = false;
        float push = 0.0f;
        bool last = false;
        bool own = false;       // an add_model model, drawn without the Rush 2049 models loaded too
        uint32_t gen = 0;
    };

    std::mutex mutex;
    Item items[rush2::battle_render::max_slots];
    std::map<std::string, uint32_t> models;   // Name (15 characters at most) -> display list address
    std::map<std::string, uint32_t> own_models;   // add_model's
    std::map<std::string, uint32_t> mine_models;  // team_mine_model's, made on first use (0: couldn't be)
    std::map<std::string, std::array<float, 3>> centers;   // model_center's, worked out on first use
    std::shared_ptr<const rush2::rom2049::Source> loaded_rom;
    bool loaded = false;

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }

    struct Chunk {
        uint32_t offset = 0, size = 0;
    };

    struct Layout {
        Chunk imag, txld, obhd, objs;
    };

    bool parse_layout(const std::vector<uint8_t>& d, Layout& out) {
        if (d.size() < 16) return false;
        for (size_t o = be32(d, 0); o + 12 <= d.size(); o += 12) {
            Chunk c{ be32(d, o + 4), be32(d, o + 8) };
            if (memcmp(&d[o], "IMAG", 4) == 0) out.imag = c;
            else if (memcmp(&d[o], "TXLD", 4) == 0) out.txld = c;
            else if (memcmp(&d[o], "OBHD", 4) == 0) out.obhd = c;
            else if (memcmp(&d[o], "OBJS", 4) == 0) out.objs = c;
        }
        return out.imag.size && out.txld.size && out.obhd.size && out.objs.size;
    }

    // Rebases one display list (and the lists it calls) in place. fixed holds the words already rebased.
    bool rebase_dl(uint8_t* rdram, uint32_t address, const Layout& layout, uint32_t file_size, uint32_t start, std::vector<bool>& fixed,
                   int depth) {
        if (depth > 8) return false;
        const uint32_t base = address & 0x1FFFFFFF;
        bool in_txld = start >= layout.txld.offset && start < layout.txld.offset + layout.txld.size;
        for (uint32_t o = start; o + 8 <= file_size; o += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(address + o));
            uint32_t w1 = (uint32_t)MEM_W(0, (int32_t)(address + o + 4));
            uint32_t op = w0 >> 24, rel = w1 & 0xFFFFFF, target = UINT32_MAX;
            if (op == 0x01 || op == 0xDE) target = rel;
            else if (op == 0xFD) target = in_txld ? rel + layout.imag.offset : rel;
            if (target != UINT32_MAX && !fixed[(o + 4) / 4]) {
                if (target >= file_size) return false;
                fixed[(o + 4) / 4] = true;
                MEM_W(0, (int32_t)(address + o + 4)) = (int32_t)(((target + base) & 0xFFFFFF) | (w1 & 0x0F000000));
                if (op == 0xDE && !rebase_dl(rdram, address, layout, file_size, rel, fixed, depth + 1)) return false;
            }
            if (op == 0xDF || (op == 0xDE && ((w0 >> 16) & 0xFF) != 0)) return true;
        }
        return false;
    }

    bool load_file(uint8_t* rdram, const rush2::rom2049::Source& rom, const ModelFile& f) {
        std::vector<uint8_t> file;
        Layout layout;
        if (!rom.read_file(f.file, file) || file.size() > f.max_size || file.size() % 4 != 0 || !parse_layout(file, layout)) {
            fprintf(stderr, "[Battle] Couldn't read Rush 2049's model file %d\n", f.file);
            return false;
        }
        for (size_t i = 0; i < file.size(); i += 4) MEM_W(0, (int32_t)(f.address + (uint32_t)i)) = (int32_t)be32(file, i);
        std::vector<bool> fixed(file.size() / 4, false);
        uint32_t file_size = (uint32_t)file.size();
        for (uint32_t i = 0; i < layout.obhd.size; i++) {
            uint32_t rec = layout.obhd.offset + i * 0x58;
            if (rec + 0x58 > file_size) return false;
            std::string name((const char*)&file[rec], strnlen((const char*)&file[rec], 15));
            uint32_t dl = be32(file, rec + 0x18 + 8);
            if (dl == 0 || dl >= file_size || !rebase_dl(rdram, f.address, layout, file_size, dl, fixed, 0)) {
                fprintf(stderr, "[Battle] Bad display list in %s\n", name.c_str());
                continue;
            }
            models[name] = f.address + dl;
        }
        // The texture load lists, called from the object lists, run back to back through the TXLD chunk.
        for (uint32_t o = layout.txld.offset; o + 8 <= layout.txld.offset + layout.txld.size; o += 8) {
            uint32_t op = (uint32_t)MEM_W(0, (int32_t)(f.address + o)) >> 24;
            uint32_t w1 = (uint32_t)MEM_W(0, (int32_t)(f.address + o + 4));
            if (op == 0xFD && !fixed[(o + 4) / 4]) {
                fixed[(o + 4) / 4] = true;
                uint32_t target = (w1 & 0xFFFFFF) + layout.imag.offset;
                MEM_W(0, (int32_t)(f.address + o + 4)) = (int32_t)(((target + (f.address & 0x1FFFFFFF)) & 0xFFFFFF) | (w1 & 0x0F000000));
            }
        }
        return true;
    }

    uint32_t side_alloc(uint32_t bytes) {
        bytes = (bytes + 63) & ~63u;
        if (side_cursor + bytes > side_end) side_cursor = side_start;
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
        void cmd(const GfxCommand& c) { cmd(c.values.word0, c.values.word1); }
        void model_group(uint32_t id) {
            GfxCommand c[2];
            gEXMatrixGroup(c, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, G_MTX_MODELVIEW_EX,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
            cmd(c[0]);
            cmd(c[1]);
        }
        void projection_group(uint32_t id) {
            GfxCommand c[2];
            gEXMatrixGroup(c, id, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, G_MTX_PROJECTION_EX,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
            cmd(c[0]);
            cmd(c[1]);
        }
        void float_matrix(uint32_t address, uint32_t params) {
            GfxCommand c[2];
            gEXMatrixFloat(c, address, params);
            cmd(c[0]);
            cmd(c[1]);
        }
        void pop_group() {
            GfxCommand c;
            gEXPopMatrixGroup(&c, G_MTX_MODELVIEW_EX);
            cmd(c);
        }
    };

    // N64 fixed point matrix (s15.16, row vectors): integer halves then fractions.
    void read_mtx(uint8_t* rdram, uint32_t addr, float m[4][4]) {
        addr = 0x80000000u | (addr & 0x00FFFFFF);
        for (int i = 0; i < 8; i++) {
            uint32_t hi = (uint32_t)MEM_W(0, (int32_t)(addr + i * 4));
            uint32_t lo = (uint32_t)MEM_W(0, (int32_t)(addr + 0x20 + i * 4));
            m[(i * 2) / 4][(i * 2) % 4] = (int32_t)((hi & 0xFFFF0000) | (lo >> 16)) / 65536.0f;
            m[(i * 2 + 1) / 4][(i * 2 + 1) % 4] = (int32_t)((hi << 16) | (lo & 0xFFFF)) / 65536.0f;
        }
    }

    void mul_mtx(const float a[4][4], const float b[4][4], float out[4][4]) {
        float r[4][4];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
        memcpy(out, r, sizeof(r));
    }

    uint32_t write_floats(uint8_t* rdram, const float m[4][4]) {
        uint32_t at = side_alloc(64);
        for (int i = 0; i < 16; i++) {
            uint32_t bits;
            memcpy(&bits, &m[i / 4][i % 4], 4);
            MEM_W(0, (int32_t)(at + i * 4)) = (int32_t)bits;
        }
        return at;
    }

    // IDs: 0101 vvgg gggg gggg gggg gggs ssss ssss. Distinct from src/interpolation.cpp's and the wings'.
    static_assert(rush2::battle_render::max_slots <= 0x200, "item_id holds 9 slot bits");
    uint32_t item_id(uint32_t view, uint32_t gen, uint32_t slot) {
        return 0x50000000u | ((view & 3) << 26) | ((gen & 0x1FFFF) << 9) | (slot & 0x1FF);
    }
}

bool rush2::battle_render::ready(uint8_t* rdram) {
    auto rom = rush2::wings::get_rom();
    std::lock_guard lock{ mutex };
    if (rom == nullptr) return false;
    if (rom != loaded_rom) {
        loaded_rom = rom;
        models.clear();
        centers.clear();
        for (const auto& [name, at] : mine_models) own_models.erase(name);
        mine_models.clear();
        loaded = true;
        for (const ModelFile& f : model_files) loaded = load_file(rdram, *rom, f) && loaded;
    }
    return loaded;
}

// The file's texture records (TXHD, 0x24 bytes: name[16], u16 width, u16 height, u8 format, u8 size, s16 palette,
// u32 texels) and palette records (PLHD, 0x18 bytes: name[16], u32, u32 colors) point into its IMAG chunk. The HUD's
// images are 4 bits a texel with a palette of 16 RGBA16 colors.
bool rush2::battle_render::image(uint8_t* rdram, const char* name, uint32_t* address, int* w, int* h) {
    struct Decoded {
        std::string name;
        uint32_t address;
        int w, h;
    };
    static std::vector<Decoded> decoded;
    static std::shared_ptr<const rush2::rom2049::Source> decoded_rom;
    static uint32_t cursor = image_start;
    std::lock_guard lock{ mutex };
    if (!loaded || loaded_rom == nullptr) return false;
    if (decoded_rom != loaded_rom) {
        decoded_rom = loaded_rom;
        decoded.clear();
        cursor = image_start;
    }
    for (const Decoded& d : decoded) {
        if (d.name == name) {
            *address = d.address;
            *w = d.w;
            *h = d.h;
            return d.address != 0;
        }
    }
    Decoded out{ name, 0, 0, 0 };
    std::vector<uint8_t> file;
    if (loaded_rom->read_file(image_file, file) && file.size() >= 16) {
        Chunk imag, txhd, plhd;
        for (size_t o = be32(file, 0); o + 12 <= file.size(); o += 12) {
            Chunk c{ be32(file, o + 4), be32(file, o + 8) };
            if (memcmp(&file[o], "IMAG", 4) == 0) imag = c;
            else if (memcmp(&file[o], "TXHD", 4) == 0) txhd = c;
            else if (memcmp(&file[o], "PLHD", 4) == 0) plhd = c;
        }
        for (uint32_t i = 0; i < txhd.size; i++) {
            size_t rec = txhd.offset + (size_t)i * 0x24;
            if (rec + 0x24 > file.size() || strncmp((const char*)&file[rec], name, 15) != 0) continue;
            int width = (file[rec + 16] << 8) | file[rec + 17], height = (file[rec + 18] << 8) | file[rec + 19];
            int palette = (int16_t)((file[rec + 22] << 8) | file[rec + 23]);
            size_t texels = imag.offset + be32(file, rec + 24);
            // A Dreamcast source's image: RGBA texels behind a load list. The converter shrinks file 63's textures to
            // the N64 file's size (HEALTHBG to 4 x 8), so the disc's full-size image is used, halved until it fits.
            std::vector<std::array<uint8_t, 4>> rgba;
            bool full = false;
            if (palette < 0) {
                for (const rush2::rom2049::SourceTexture& t : loaded_rom->file_textures(image_file)) {
                    std::vector<uint8_t> pixels;
                    int fw = 0, fh = 0;
                    if (t.name != name || !loaded_rom->source_image(t, pixels, fw, fh) || fw <= 0 || fh <= 0) continue;
                    while (fw * fh * 2 > (int)(image_end - cursor) && fw > 1 && fh > 1) {
                        std::vector<uint8_t> half((size_t)(fw / 2) * (fh / 2) * 4);
                        for (int y = 0; y < fh / 2; y++) {
                            for (int x = 0; x < fw / 2; x++) {
                                for (int c = 0; c < 4; c++) {
                                    int sum = 0;
                                    for (int k = 0; k < 4; k++) sum += pixels[((size_t)(y * 2 + k / 2) * fw + x * 2 + k % 2) * 4 + c];
                                    half[((size_t)y * (fw / 2) + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                                }
                            }
                        }
                        pixels = std::move(half);
                        fw /= 2;
                        fh /= 2;
                    }
                    rgba.resize((size_t)fw * fh);
                    for (size_t p = 0; p < rgba.size(); p++) rgba[p] = { pixels[p * 4], pixels[p * 4 + 1], pixels[p * 4 + 2], pixels[p * 4 + 3] };
                    width = fw;
                    height = fh;
                    full = true;
                    break;
                }
            }
            if (palette < 0 && (full || rush2::rom2049::list_image(file, imag.offset, (uint32_t)rec, rgba, width, height))) {
                uint32_t bytes = (uint32_t)(width * height * 2);
                if (cursor + bytes > image_end) break;
                for (int t = 0; t < width * height; t++) {
                    const auto& c = rgba[t];
                    MEM_H(t * 2, (int32_t)cursor) = (int16_t)((c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | (c[3] >= 128 ? 1 : 0));
                }
                out.address = cursor;
                out.w = width;
                out.h = height;
                cursor += (bytes + 7) & ~7u;
                break;
            }
            size_t pal_rec = plhd.offset + (size_t)palette * 0x18;
            if (palette < 0 || (uint32_t)palette >= plhd.size || pal_rec + 0x18 > file.size() || width <= 0 || height <= 0) break;
            size_t colors = imag.offset + be32(file, pal_rec + 20);
            uint32_t bytes = (uint32_t)(width * height * 2);
            if (texels + (size_t)(width * height + 1) / 2 > file.size() || colors + 32 > file.size() || cursor + bytes > image_end) break;
            for (int t = 0; t < width * height; t++) {
                int index = (file[texels + t / 2] >> ((t & 1) ? 0 : 4)) & 0xF;
                MEM_H(t * 2, (int32_t)cursor) = (int16_t)((file[colors + index * 2] << 8) | file[colors + index * 2 + 1]);
            }
            out.address = cursor;
            out.w = width;
            out.h = height;
            cursor += (bytes + 7) & ~7u;
            break;
        }
    }
    if (out.address == 0) fprintf(stderr, "[Battle] Couldn't read Rush 2049's image %s\n", name);
    decoded.push_back(out);
    *address = out.address;
    *w = out.w;
    *h = out.h;
    return out.address != 0;
}

namespace {
    // Adds the vertices display list dl loads, and those of the lists it calls, to out (model units, 1/16 ft).
    // After the load the lists' addresses are physical (rebase_dl); vertices are s16 x, y, z first of 16 bytes.
    void dl_vertices(uint8_t* rdram, uint32_t dl, std::vector<std::array<float, 3>>& out, int depth) {
        if (depth > 8) return;
        for (int n = 0; n < 4096; n++, dl += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)dl), w1 = (uint32_t)MEM_W(0, (int32_t)(dl + 4));
            uint32_t op = w0 >> 24, target = 0x80000000u | (w1 & 0xFFFFFF);
            if (op == 0x01) {
                int count = (int)((w0 >> 12) & 0xFF);
                for (int v = 0; v < count; v++) {
                    std::array<float, 3> p;
                    for (int k = 0; k < 3; k++) p[k] = (float)(int16_t)MEM_H(0, (int32_t)(target + (uint32_t)v * 16 + (uint32_t)k * 2));
                    out.push_back(p);
                }
            }
            else if (op == 0xDE) {
                dl_vertices(rdram, target, out, depth + 1);
                if (((w0 >> 16) & 0xFF) != 0) return;   // a branch: no return
            }
            else if (op == 0xDF) {
                return;
            }
        }
    }
}

bool rush2::battle_render::model_center(uint8_t* rdram, const char* name, float out[3]) {
    std::lock_guard lock{ mutex };
    std::string key = std::string(name).substr(0, 15);
    auto c = centers.find(key);
    if (c == centers.end()) {
        auto m = models.find(key);
        if (m == models.end()) return false;
        std::vector<std::array<float, 3>> points;
        dl_vertices(rdram, m->second, points, 0);
        float box[6] = { 1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f };
        for (const auto& p : points) {
            for (int k = 0; k < 3; k++) {
                box[k] = std::min(box[k], p[k]);
                box[3 + k] = std::max(box[3 + k], p[k]);
            }
        }
        std::array<float, 3> mid = { 0.0f, 0.0f, 0.0f };
        if (box[0] <= box[3]) {
            for (int k = 0; k < 3; k++) mid[k] = (box[k] + box[3 + k]) * 0.5f / 16.0f;   // vertices are 1/16 ft
        }
        c = centers.emplace(key, mid).first;
    }
    for (int k = 0; k < 3; k++) out[k] = c->second[k];
    return true;
}

bool rush2::battle_render::model_vertices(uint8_t* rdram, const char* name, std::vector<std::array<float, 3>>& out) {
    std::lock_guard lock{ mutex };
    out.clear();
    auto m = models.find(std::string(name).substr(0, 15));
    if (m == models.end()) return false;
    dl_vertices(rdram, m->second, out, 0);
    for (auto& p : out) {
        for (float& v : p) v /= 16.0f;   // vertices are 1/16 ft
    }
    return !out.empty();
}

// Rush 2049's dropped mine (func_8038E088's caller, 0x8038E3F4 in the battle overlay): two nodes, WPR_MINEBASE (the
// open bowl, with its own texture load) and a child WPR_MINECAP (the dome, which loads no texture of its own) given the
// texture MINE_CAP_B, _R, _Y or _G by the owner's team (0x8038E45C: the texture handles at 0x80399AF8, read through
// the team's color index, 0x8012E67C). Drawn here as one list: the base, the team's cap texture, the cap. The cap's
// texture load is a copy of MINE_CAP's own (TXLD) with its image and palette swapped for the team's, all four being
// 32 x 32 CI8 with a 256-color palette.
std::string rush2::battle_render::team_mine_model(uint8_t* rdram, int team) {
    static const char* const caps[4] = { "MINE_CAP_B", "MINE_CAP_R", "MINE_CAP_Y", "MINE_CAP_G" };
    std::lock_guard lock{ mutex };
    std::string name = "WPR_MINE_" + std::to_string(team & 3);
    if (mine_models.count(name)) return mine_models[name] != 0 ? name : std::string();
    mine_models[name] = 0;
    auto base = models.find("WPR_MINEBASE"), cap = models.find("WPR_MINECAP");
    if (!loaded || loaded_rom == nullptr || base == models.end() || cap == models.end()) return {};
    std::vector<uint8_t> file;
    Layout layout;
    const ModelFile& f = model_files[0];   // file 76
    if (!loaded_rom->read_file(f.file, file) || !parse_layout(file, layout)) return {};
    Chunk txhd, plhd;
    for (size_t o = be32(file, 0); o + 12 <= file.size(); o += 12) {
        if (memcmp(&file[o], "TXHD", 4) == 0) txhd = { be32(file, o + 4), be32(file, o + 8) };
        else if (memcmp(&file[o], "PLHD", 4) == 0) plhd = { be32(file, o + 4), be32(file, o + 8) };
    }
    // A texture's image and palette, IMAG relative; false unless it is image data (flag 0x08000000) with a palette.
    auto image_of = [&](const char* tex, uint32_t& texels, uint32_t& colors) {
        for (uint32_t i = 0; i < txhd.size; i++) {
            size_t rec = txhd.offset + (size_t)i * 0x24;
            if (rec + 0x24 > file.size() || strncmp((const char*)&file[rec], tex, 16) != 0) continue;
            int palette = (int16_t)((file[rec + 22] << 8) | file[rec + 23]);
            size_t pal_rec = plhd.offset + (size_t)palette * 0x18;
            if (!(be32(file, rec + 28) & 0x08000000) || palette < 0 || (uint32_t)palette >= plhd.size || pal_rec + 0x18 > file.size()) return false;
            texels = be32(file, rec + 24);
            colors = be32(file, pal_rec + 20);
            return true;
        }
        return false;
    };
    uint32_t plain_texels, plain_colors, team_texels, team_colors;
    if (!image_of("MINE_CAP", plain_texels, plain_colors) || !image_of(caps[team & 3], team_texels, team_colors)) return {};
    // MINE_CAP's load list: the TXLD list whose image is MINE_CAP's (the lists run back to back, each to its G_ENDDL).
    uint32_t list = 0, list_size = 0;
    for (uint32_t start = layout.txld.offset, o = start; o + 8 <= layout.txld.offset + layout.txld.size && o + 8 <= file.size(); o += 8) {
        uint32_t op = file[o];
        if (op == 0xFD && list == 0 && (be32(file, o + 4) & 0xFFFFFF) == plain_texels) list = start;
        if (op == 0xDF) {
            if (list != 0) {
                list_size = o + 8 - list;
                break;
            }
            start = o + 8;
        }
    }
    if (list == 0 || list_size == 0) return {};
    uint32_t imag = (f.address & 0x1FFFFFFF) + layout.imag.offset;
    // The lists live in the last 0x400 bytes of the file's room (side_alloc's space is reused once it wraps).
    uint32_t at = f.address + f.max_size - 0x400 + uint32_t(team & 3) * 0x100;
    if (list_size + 3 * 8 > 0x100 || file.size() > f.max_size - 0x400) return {};
    uint32_t w = at;
    auto put = [&](uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)w) = (int32_t)w0;
        MEM_W(4, (int32_t)w) = (int32_t)w1;
        w += 8;
    };
    put(G_DL_CALL, base->second & 0x1FFFFFFF);
    for (uint32_t o = list; o < list + list_size - 8; o += 8) {
        uint32_t w0 = be32(file, o), w1 = be32(file, o + 4);
        if ((w0 >> 24) == 0xFD) {
            uint32_t rel = w1 & 0xFFFFFF;
            if (rel == plain_texels) rel = team_texels;
            else if (rel == plain_colors) rel = team_colors;
            w1 = (w1 & 0x0F000000) | ((imag + rel) & 0xFFFFFF);
        }
        put(w0, w1);
    }
    put(G_DL_CALL, cap->second & 0x1FFFFFFF);
    put(G_ENDDL_W0, 0);
    mine_models[name] = at;
    own_models[name] = at;
    return name;
}

std::string rush2::battle_render::find_model(const char* prefix) {
    std::lock_guard lock{ mutex };
    auto it = models.lower_bound(prefix);
    return it != models.end() && it->first.compare(0, strlen(prefix), prefix) == 0 ? it->first : std::string();
}

bool rush2::battle_render::has_model(const char* name) {
    std::lock_guard lock{ mutex };
    return models.find(std::string(name).substr(0, 15)) != models.end();
}

void rush2::battle_render::place(int slot, const char* model, const float m[9], const float pos[3], uint32_t rgba, int view, bool attached,
                                 bool billboard, float push, bool last, bool same_object) {
    std::lock_guard lock{ mutex };
    if (slot < 0 || slot >= max_slots) return;
    Item& it = items[slot];
    uint32_t dl = 0;
    bool own_model = false;
    if (auto found = models.find(std::string(model).substr(0, 15)); found != models.end()) dl = found->second;
    else if (auto own = own_models.find(model); own != own_models.end()) dl = own->second, own_model = true;
    if (dl == 0) {
        it.on = false;
        return;
    }
    float d[3] = { pos[0] - it.pos[0], pos[1] - it.pos[1], pos[2] - it.pos[2] };
    // A new use of the slot, or a jump: nothing to move it from on the frames in between.
    if (!it.on || (it.dl != dl && !same_object) || (!attached && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > teleport_distance * teleport_distance)) {
        it.gen++;
    }
    it.on = true;
    it.dl = dl;
    memcpy(it.m, m, sizeof(it.m));
    memcpy(it.pos, pos, sizeof(it.pos));
    it.rgba = rgba;
    it.view = view;
    it.attached = attached;
    it.billboard = billboard;
    it.push = push;
    it.last = last;
    it.own = own_model;
}

void rush2::battle_render::add_model(const char* name, uint32_t dl) {
    std::lock_guard lock{ mutex };
    own_models[name] = dl;
}

void rush2::battle_render::hide(int slot) {
    std::lock_guard lock{ mutex };
    if (slot >= 0 && slot < max_slots) items[slot].on = false;
}

void rush2::battle_render::hide_in_view(int slot, int view) {
    std::lock_guard lock{ mutex };
    if (slot >= 0 && slot < max_slots) items[slot].hidden_view = view;
}

void rush2::battle_render::clear() {
    std::lock_guard lock{ mutex };
    for (Item& it : items) it.on = false;
}

// func_8007C624's exit: the placed models of the view just drawn.
extern "C" void rush2_battle_render_view(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    // The menus draw 3D views too (the title's logo). Their users place these models from a race's ticks, which stop
    // when it ends, so a race left for the menus (the attract demo going back to the title) would keep its last ones.
    if ((int32_t)MEM_W(0, (int32_t)game_state) < 3) {
        for (Item& it : items) it.on = false;
        return;
    }
    uint32_t view = 0, gen = 0, root = 0, projection = 0, lookat = 0;
    rush2_interp_get_generation(&view, &gen);
    if (!rush2_interp_view_matrices(view, &root, &projection, &lookat)) return;
    // Without the Rush 2049 models loaded (no ROM) only the port's own models draw.
    auto drawn = [&](const Item& it) {
        return it.on && (loaded || it.own) && (it.view < 0 || it.view == (int)view) && it.hidden_view != (int)view;
    };
    int count = 0, attached = 0;
    for (const Item& it : items) {
        if (drawn(it)) {
            count++;
            attached += it.attached;
        }
    }
    if (count == 0) return;

    float cam[3];
    for (int i = 0; i < 3; i++) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)(cameras + view * 0x40 + 0x24 + i * 4));
        memcpy(&cam[i], &w, 4);
    }
    // The view's axes for the billboards: an explosion frame is a card in its xy plane, seen from the camera along its
    // +z (the other way it is culled).
    float cam_axes[9];
    rush2::views::axes(rdram, (int)view, cam_axes);
    float root_m[4][4], view_m[4][4];
    read_mtx(rdram, root, root_m);
    read_mtx(rdram, lookat, view_m);
    mul_mtx(root_m, view_m, view_m);

    DlWriter dl{ rdram, side_alloc((uint32_t)(32 + count * 10) * 8) };
    for (const auto& c : prologue) dl.cmd(c[0], c[1]);
    dl.cmd(G_MTX_LOAD_MODELVIEW_W0, root);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (attached == 0) break;
            dl.projection_group(0x21000000u | (view << 16));
            dl.cmd(G_MTX_LOAD_PROJECTION_W0, projection);
        }
        // The models marked last after the others: a see-through model that writes depth hides what is drawn after it.
        for (int order = 0; order < rush2::battle_render::max_slots * 2; order++) {
            int slot = order % rush2::battle_render::max_slots;
            const Item& it = items[slot];
            if (!drawn(it) || it.attached != (pass == 1)) continue;
            if (it.last != (order >= rush2::battle_render::max_slots)) continue;
            float m[4][4] = {};
            float rel[3] = { it.pos[0] - cam[0], it.pos[1] - cam[1], it.pos[2] - cam[2] };
            if (it.push != 0.0f) {
                float len = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
                if (len > 0.001f) {
                    for (float& v : rel) v += v / len * it.push;
                }
            }
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) m[r][c] = it.m[r * 3 + c];
                m[3][r] = rel[r] * 16.0f;
            }
            if (it.billboard) {
                float scale = std::sqrt(it.m[0] * it.m[0] + it.m[1] * it.m[1] + it.m[2] * it.m[2]);
                for (int c = 0; c < 3; c++) {
                    m[0][c] = cam_axes[c] * scale;
                    m[1][c] = cam_axes[3 + c] * scale;
                    m[2][c] = cam_axes[6 + c] * scale;
                }
            }
            m[3][3] = 1.0f;
            dl.model_group(item_id(view, gen + it.gen, (uint32_t)slot));
            if (pass == 1) {
                mul_mtx(m, view_m, m);
                dl.float_matrix(write_floats(rdram, m), 0x02);   // push, load (the push bit is stored inverted)
            }
            else {
                dl.float_matrix(write_floats(rdram, m), 0x00);   // push, multiply
            }
            dl.cmd(0xFA000000, it.rgba);
            dl.cmd(G_DL_CALL, it.dl);
            dl.cmd(G_POPMTX_W0, 0x40);
            dl.pop_group();
        }
    }
    if (attached != 0) {
        // The view's projection again, with its rotation.
        dl.projection_group(0x20000000u | (view << 16) | (gen & 0xFFFF));
        dl.cmd(G_MTX_LOAD_PROJECTION_W0, projection);
        dl.cmd(G_MTX_MUL_PROJECTION_W0, lookat);
    }
    for (const auto& c : epilogue) dl.cmd(c[0], c[1]);
    if (MEM_BU(0, (int32_t)lighting_cache) != 0) dl.cmd(0xD9FFFFFF, 0x00020000);
    // Test aid: R2_BATTLE_TEST=dlfault loads unknown microcode once (G_LOAD_UCODE of address 0), to check that RT64
    // ends the list and writes dl_fault.txt (docs/crash_reports.md#display-list-faults).
    static const bool test_fault = getenv("R2_BATTLE_TEST") != nullptr && strstr(getenv("R2_BATTLE_TEST"), "dlfault") != nullptr;
    static int fault_countdown = 600;   // views drawn with battle models first: about 10 s of racing
    if (test_fault && fault_countdown > 0 && --fault_countdown == 0) dl.cmd(0xDD000000, 0);
    MEM_W(0, (int32_t)palette_cache) = 0;
    dl.cmd(G_ENDDL_W0, 0);

    uint32_t head = (uint32_t)MEM_W(0, (int32_t)main_dl_head);
    MEM_W(0, (int32_t)head) = (int32_t)G_DL_CALL;
    MEM_W(4, (int32_t)head) = (int32_t)dl.addr;
    MEM_W(0, (int32_t)main_dl_head) = (int32_t)(head + 8);
}
