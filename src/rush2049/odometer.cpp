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
#include <mutex>
#include <string>
#include <thread>
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

    constexpr int cell = 8;                         // a digit's cell, texels and screen pixels
    constexpr int column_h = 11 * cell;             // 0-9 and 0
    constexpr uint32_t whole_image = 0x80C9A000;    // 8 x 88 RGBA16 each
    constexpr uint32_t tenths_image = 0x80C9A600;
    constexpr float max_step = 300.0f;

    std::atomic_bool option = false;

    // The digit columns made from one source's ODOMETER: upright RGBA16, cell x column_h each. Made off the game thread
    // (prepare): a Dreamcast source converts the disc's HUDCOM container for it, long enough to show as a hitch.
    struct Image {
        std::shared_ptr<const rush2::rom2049::Source> rom;
        bool ok = false;
        std::vector<uint16_t> columns[2];
    };
    std::mutex image_mutex;
    std::shared_ptr<const Image> image;             // under image_mutex
    std::atomic_bool loading = false;
    std::shared_ptr<const Image> written;           // the image whose columns are in RDRAM (game thread)

    float distance[4];
    float last_pos[4][3];
    bool has_last[4];

    // ODOMETER from the HUD file as RGBA, upright. The N64's is 16 x 88. A Dreamcast source's converted file has it
    // shrunk to fit TMEM (32 x 64, too short for the 11 cells), so the disc's full-size image is used: 32 x 256, rows
    // stored bottom up like the N64's, the 176 rows of digits on top once upright.
    bool read_image(const rush2::rom2049::Source& rom, std::vector<std::array<uint8_t, 4>>& rgba, int& w, int& h) {
        if (rom.is_dreamcast()) {
            for (const rush2::rom2049::SourceTexture& t : rom.file_textures(rush2::rom2049::hud_file)) {
                std::vector<uint8_t> px;
                if (t.name != "ODOMETER" || !rom.source_image(t, px, w, h) || w <= 0 || h <= 0) continue;
                rgba.resize((size_t)(w * h));
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const uint8_t* p = &px[((size_t)(h - 1 - y) * w + x) * 4];
                        rgba[(size_t)(y * w + x)] = { p[0], p[1], p[2], p[3] };
                    }
                }
                return true;
            }
            return false;
        }
        std::vector<uint8_t> file;
        return rom.read_file(rush2::rom2049::hud_file, file) && rush2::rom2049::file_image(file, "ODOMETER", rgba, w, h);
    }

    // The two columns of rom's ODOMETER, averaged down to the N64's size.
    std::shared_ptr<const Image> make_image(std::shared_ptr<const rush2::rom2049::Source> rom) {
        auto made = std::make_shared<Image>();
        made->rom = rom;
        std::vector<std::array<uint8_t, 4>> rgba;
        int w = 0, h = 0;
        if (rom == nullptr || !read_image(*rom, rgba, w, h) || w < 2 * cell || h < column_h) {
            if (rom != nullptr) fprintf(stderr, "[Odometer] Couldn't read Rush 2049's ODOMETER image\n");
            return made;
        }
        int fx = w / (2 * cell), fy = h / column_h;
        for (int c = 0; c < 2; c++) {
            made->columns[c].assign((size_t)(cell * column_h), 0);
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
                    made->columns[c][(size_t)(y * cell + x)] = (uint16_t)((sum[0] / n >> 3) << 11 | (sum[1] / n >> 3) << 6 |
                                                                          (sum[2] / n >> 3) << 1 | (sum[3] / n >= 128 ? 1 : 0));
                }
            }
        }
        made->ok = true;
        return made;
    }

    // The image for the current Rush 2049 source, or null while it is being made (on a worker, never the game thread).
    std::shared_ptr<const Image> current_image() {
        auto rom = rush2::wings::get_rom();
        std::shared_ptr<const Image> have;
        {
            std::lock_guard lock{ image_mutex };
            have = image;
        }
        if (have != nullptr && have->rom == rom) return have;
        if (!loading.exchange(true)) {
            std::thread([rom] {
                rush2::odometer2049::prepare(rom);
                loading = false;
            }).detach();
        }
        return nullptr;
    }

    void write_images(uint8_t* rdram, const std::shared_ptr<const Image>& img) {
        for (int c = 0; c < 2; c++) {
            uint32_t address = c == 0 ? whole_image : tenths_image;
            for (int t = 0; t < cell * column_h; t++) MEM_H(t * 2, (int32_t)address) = (int16_t)img->columns[c][(size_t)t];
        }
        written = img;
    }
}

void rush2::odometer2049::prepare(std::shared_ptr<const rush2::rom2049::Source> rom) {
    {
        std::lock_guard lock{ image_mutex };
        if (image != nullptr && image->rom == rom) return;
    }
    auto made = make_image(rom);
    std::lock_guard lock{ image_mutex };
    image = std::move(made);
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
    written = nullptr;
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
    auto img = current_image();
    if (img == nullptr || !img->ok) return;
    if (written != img) write_images(rdram, img);

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
