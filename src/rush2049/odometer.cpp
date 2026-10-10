// Rush 2049's odometer (include/odometer2049.h), under each player's race time when the Odometer option is on.
//
// Rush 2049 (N64 USA) draws it with four HUD widgets per player, layout entries 0x80114E50-0x8011506C (image
// "ODOMETER", callback func_80106D94, parameter player << 4 | digit). The callback reads the car's distance, car
// state +0x108 (0x80152818 + car x 0x3B8), in feet: func_800F8EC8 (the checkpoint gate test, run every physics step)
// adds the length of the car's move since the last step to it. With the metric flag 0x80146111 set it divides by 0.6
// (km). Digit 0-3 is hundreds, tens and units of miles and tenths: each shows (distance / 528000, 52800, 5280,
// 528) mod 10, a cell 8 texels tall of the image, and a digit rolls into the next one by the tenths' fraction while
// every digit after it reads 9; the tenths roll all the time. The digits sit at x - 16, x - 8, x and x + 8 of the
// player's position (table 0x80115AE8: {x, y} per player for 1-4 players; (30, 32) alone, under the race time), the
// tenths from the image's right column.
//
// ODOMETER is in Rush 2049's HUD file 64: 16 x 88, 4 bits a texel with a 16-color palette, rows stored bottom up (as
// all of 2049's images). Upright, each column holds 0-9 and 0 again in cells of 8 x 8: whole digits white on black on
// the left, the tenths black on white on the right. The two columns are split into two images in spare RDRAM so each is
// clamped at its own edges (src/controls_menu.cpp lists the ranges in use).
//
// Rush 2 keeps no distance, so this file keeps it per player from the position each car is drawn at (car state +0,
// 0x801124A0 + car x 0x354), once per frame. A frame's move past 300 feet (about 6000 mph) isn't counted: it is the car
// being put on the grid as the race starts, not driving.

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "recomp.h"

#include "rush2.h"
#include "arrows.h"
#include "odometer2049.h"
#include "rush2049_rom.h"
#include "wings.h"

namespace {
    constexpr uint32_t car_states = 0x801124A0;     // 0x354 each: +0 the position the car is drawn at
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t metric_flag = 0x800D4160;    // u8: the speedometer reads km/h (func_800BA2A8)
    constexpr uint32_t game_state = 0x8010C0D0;     // 0-2 the menus, 3 and up a race

    constexpr int hud_file = 64;
    constexpr int cell = 8;                         // a digit's cell, texels and screen pixels
    constexpr int column_h = 11 * cell;             // 0-9 and 0
    constexpr uint32_t whole_image = 0x80C9A000;    // 8 x 88 RGBA16 each
    constexpr uint32_t tenths_image = 0x80C9A600;
    constexpr float max_step = 300.0f;

    std::atomic_bool option = false;

    std::shared_ptr<const rush2::rom2049::Source> loaded_rom;
    bool image_ok = false;
    std::vector<uint16_t> columns[2];               // upright RGBA16, cell x column_h each
    bool written = false;                           // columns are in RDRAM

    float distance[4];
    float last_pos[4][3];
    bool has_last[4];

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
    }
    uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
        return (uint16_t)(d[o] << 8 | d[o + 1]);
    }

    // ODOMETER from the HUD file as RGBA, upright. A Dreamcast source's image is RGBA texels behind a load list
    // (bottom up as well).
    bool read_image(const rush2::rom2049::Source& rom, std::vector<std::array<uint8_t, 4>>& rgba, int& w, int& h) {
        std::vector<uint8_t> file;
        if (!rom.read_file(hud_file, file) || file.size() < 4) return false;
        uint32_t imag = 0, txhd = 0, txhd_n = 0, plhd = 0, plhd_n = 0;
        for (size_t o = be32(file, 0); o + 12 <= file.size(); o += 12) {
            if (memcmp(&file[o], "IMAG", 4) == 0) imag = be32(file, o + 4);
            else if (memcmp(&file[o], "TXHD", 4) == 0) { txhd = be32(file, o + 4); txhd_n = be32(file, o + 8); }
            else if (memcmp(&file[o], "PLHD", 4) == 0) { plhd = be32(file, o + 4); plhd_n = be32(file, o + 8); }
        }
        for (uint32_t i = 0; i < txhd_n; i++) {
            size_t rec = txhd + (size_t)i * 0x24;
            if (rec + 0x24 > file.size() || strncmp((const char*)&file[rec], "ODOMETER", 16) != 0) continue;
            w = be16(file, rec + 16);
            h = be16(file, rec + 18);
            int palette = (int16_t)be16(file, rec + 22);
            std::vector<std::array<uint8_t, 4>> rows;
            if (palette < 0) {
                if (!rush2::rom2049::list_image(file, imag, (uint32_t)rec, rows, w, h)) return false;
            }
            else {
                size_t texels = imag + be32(file, rec + 24);
                size_t pal_rec = plhd + (size_t)palette * 0x18;
                if ((uint32_t)palette >= plhd_n || pal_rec + 0x18 > file.size() || w <= 0 || h <= 0) return false;
                size_t colors = imag + be32(file, pal_rec + 20);
                if (texels + (size_t)(w * h + 1) / 2 > file.size() || colors + 32 > file.size()) return false;
                rows.resize((size_t)(w * h));
                for (int t = 0; t < w * h; t++) {
                    int index = (file[texels + t / 2] >> ((t & 1) ? 0 : 4)) & 0xF;
                    uint16_t c = be16(file, colors + index * 2);
                    rows[t] = { uint8_t((c >> 11) << 3), uint8_t(((c >> 6) & 31) << 3), uint8_t(((c >> 1) & 31) << 3),
                                uint8_t((c & 1) ? 255 : 0) };
                }
            }
            rgba.resize((size_t)(w * h));
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) rgba[(size_t)(y * w + x)] = rows[(size_t)((h - 1 - y) * w + x)];
            }
            return w > 0 && h > 0;
        }
        return false;
    }

    // Splits the image into its two columns at the N64's size (a larger Dreamcast image is averaged down).
    void load_image() {
        auto rom = rush2::wings::get_rom();
        if (rom == loaded_rom) return;
        loaded_rom = rom;
        image_ok = false;
        written = false;
        std::vector<std::array<uint8_t, 4>> rgba;
        int w = 0, h = 0;
        if (rom == nullptr || !read_image(*rom, rgba, w, h) || w < 2 * cell || h < column_h) {
            if (rom != nullptr) fprintf(stderr, "[Odometer] Couldn't read Rush 2049's ODOMETER image\n");
            return;
        }
        int fx = w / (2 * cell), fy = h / column_h;
        for (int c = 0; c < 2; c++) {
            columns[c].assign((size_t)(cell * column_h), 0);
            for (int y = 0; y < column_h; y++) {
                for (int x = 0; x < cell; x++) {
                    int sum[4] = {};
                    for (int sy = 0; sy < fy; sy++) {
                        for (int sx = 0; sx < fx; sx++) {
                            const auto& p = rgba[(size_t)((y * fy + sy) * w + (c * cell + x) * fx + sx)];
                            for (int k = 0; k < 4; k++) sum[k] += p[k];
                        }
                    }
                    int n = fx * fy;
                    columns[c][(size_t)(y * cell + x)] = (uint16_t)((sum[0] / n >> 3) << 11 | (sum[1] / n >> 3) << 6 |
                                                                    (sum[2] / n >> 3) << 1 | (sum[3] / n >= 128 ? 1 : 0));
                }
            }
        }
        image_ok = true;
    }

    void write_images(uint8_t* rdram) {
        for (int c = 0; c < 2; c++) {
            uint32_t address = c == 0 ? whole_image : tenths_image;
            for (int t = 0; t < cell * column_h; t++) MEM_H(t * 2, (int32_t)address) = (int16_t)columns[c][(size_t)t];
        }
        written = true;
    }
}

void rush2::odometer2049::set_option(bool on) {
    option = on;
}

bool rush2::odometer2049::enabled() {
    return option.load(std::memory_order_relaxed) && rush2::wings::rom_available();
}

void rush2::odometer2049::reset() {
    for (int p = 0; p < 4; p++) {
        distance[p] = 0.0f;
        has_last[p] = false;
    }
    written = false;
}

void rush2::odometer2049::update(uint8_t* rdram) {
    if (MEM_W(0, (int32_t)game_state) < 3) {
        for (bool& h : has_last) h = false;
        return;
    }
    int players = rush2::views::local_players(rdram);
    for (int p = 0; p < 4; p++) {
        int car = p < players ? rush2::views::player_car(rdram, p) : -1;
        if (car < 0 || car > 7) {
            has_last[p] = false;
            continue;
        }
        uint32_t state = car_states + (uint32_t)car * car_state_size;
        float pos[3];
        for (int i = 0; i < 3; i++) {
            uint32_t bits = (uint32_t)MEM_W(i * 4, (int32_t)state);
            memcpy(&pos[i], &bits, 4);
        }
        if (has_last[p]) {
            float dx = pos[0] - last_pos[p][0], dy = pos[1] - last_pos[p][1], dz = pos[2] - last_pos[p][2];
            float step = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(step) && step <= max_step) distance[p] += step;
        }
        memcpy(last_pos[p], pos, sizeof(pos));
        has_last[p] = true;
    }
}

// func_80106D94's digits.
void rush2::odometer2049::draw(uint8_t* rdram, int player, float x, float y, float scale, float anchor) {
    if (player < 0 || player > 3) return;
    load_image();
    if (!image_ok) return;
    if (!written) write_images(rdram);

    float d = distance[player];
    if (MEM_B(0, (int32_t)metric_flag) != 0) d /= 0.6f;
    float tenths = d / 528.0f;
    int tenths_int = (int)tenths;
    int tenth = tenths_int % 10;
    float tenth_fraction = tenths - (float)tenths_int;
    // The whole digits roll while the tenths read 9 and every whole digit after them reads 9.
    float roll = tenth == 9 ? tenth_fraction : 0.0f;
    const float divisors[3] = { 528000.0f, 52800.0f, 5280.0f };
    int digits[3];
    for (int i = 0; i < 3; i++) digits[i] = (int)(d / divisors[i]) % 10;
    for (int i = 0; i < 4; i++) {
        int t;
        if (i < 3) {
            bool rolling = tenth == 9;
            for (int j = i + 1; j < 3; j++) rolling = rolling && digits[j] == 9;
            t = digits[i] * cell + (rolling ? (int)(roll * cell) : 0);
        }
        else {
            t = (int)(((float)tenth + tenth_fraction) * cell);
        }
        // Shrunk, a digit is a whole number of pixels (rush2::hud::draw_image_part point samples it at that size).
        float size = scale < 1.0f ? std::round(cell * scale) : cell * scale;
        float cx = x + (float)i * size;
        rush2::hud::draw_image_part(rdram, i < 3 ? whole_image : tenths_image, cell, column_h, 0, t, cell, cell,
                                    cx, y, cx + size, y + size, anchor, scale < 1.0f);
    }
}

// Start of func_800A5110 (0x800A5114), the race setup, also on a restart.
extern "C" void rush2_odometer_race_setup(uint8_t* rdram, recomp_context* ctx) {
    rush2::odometer2049::reset();
}
