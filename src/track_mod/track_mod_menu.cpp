// Custom tracks' track select art (include/track_mod.h, docs/custom_tracks.md).
//
// The track select (src/rush2049/track2049_menu.cpp) looks its dioramas and logos up by name in asset 3, which already
// gets the Rush 2049 and SF Rush tracks' art appended. A custom track adds its diorama as model MODTRK<nn> and its logo
// as texture MODLOGO<nn> (nn = its index in the enabled mods' tracks) through rush2::track2049::extend_menu_container,
// the code that adds the 2049 tracks' art. The diorama is the author's ([menu] diorama, converted with the track);
// without one it is Rush 2049's track select preview of the AI path (rush2::track2049::route_preview: the route tube
// with the moving highlight, in front of the track's [menu] preview picture when it has one), posed and animated as
// the 2049 tracks' are. The logo is the [menu] logo picture fitted into Rush 2's 128 x 32 logo space; without one it
// is clear.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "texture_upscale.h"
#include "track2049.h"
#include "track_mod.h"

namespace {
    using Bytes = std::vector<uint8_t>;
    constexpr int logo_w = 128, logo_h = 32;
    constexpr int picture_size = 128;     // Rush 2049's track select screenshots are 128 x 128

    uint32_t get32(const Bytes& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    void add32(Bytes& d, uint32_t v) { for (int s = 24; s >= 0; s -= 8) d.push_back(uint8_t(v >> s)); }

    // The picture in `file` fitted into w x h (aspect kept, centered, clear around it), RGBA rows top-down; empty when
    // there is none or it can't be read. Notes what it did in the report.
    Bytes fit_picture(const Bytes& file, uint32_t w, uint32_t h, const std::string& what, rush2::track_mod::Report& report) {
        rush2::upscale::Image img;
        if (file.empty()) return {};
        if (!rush2::upscale::decode_image(file, img) || img.empty()) {
            report.warning("The " + what + " picture can't be read (PNG expected); it is left out.");
            return {};
        }
        if (img.width * h != img.height * w) {
            report.info("The " + what + " picture is " + std::to_string(img.width) + " x " + std::to_string(img.height) +
                        "; it is fitted into " + std::to_string(w) + " x " + std::to_string(h) + " with the rest left clear.");
        }
        float s = std::min(float(w) / float(img.width), float(h) / float(img.height));
        uint32_t fw = std::clamp(uint32_t(std::lround(img.width * s)), 1u, w);
        uint32_t fh = std::clamp(uint32_t(std::lround(img.height * s)), 1u, h);
        rush2::upscale::Image fitted = s < 1.0f ? rush2::upscale::shrink(img, fw, fh) : rush2::upscale::resize_bilinear(img, fw, fh);
        Bytes rgba(size_t(w) * h * 4, 0);
        uint32_t x0 = (w - fw) / 2, y0 = (h - fh) / 2;
        for (uint32_t y = 0; y < fh; y++) {
            memcpy(&rgba[(size_t(y0 + y) * w + x0) * 4], &fitted.rgba[size_t(y) * fw * 4], size_t(fw) * 4);
        }
        return rgba;
    }
}

std::string rush2::track_mod::diorama_model_name(int index) {
    char b[16];
    snprintf(b, sizeof(b), "MODTRK%02d", index);
    return b;
}

std::string rush2::track_mod::logo_texture_name(int index) {
    char b[16];
    snprintf(b, sizeof(b), "MODLOGO%02d", index);
    return b;
}

void rush2::track_mod::make_menu_art(const Converted& track, const Extras& extras, MenuArt& art, Report& report) {
    // Logo: CI8 as the previews' screenshots are quantized (index 255 clear), rows bottom-up.
    Bytes rgba = fit_picture(extras.logo, logo_w, logo_h, "logo", report);
    art.logo.assign(size_t(logo_w * logo_h), 255);
    art.logo_palette.assign(512, 0);
    art.logo_rgba.assign(size_t(logo_w * logo_h * 4), 0);
    if (rgba.empty()) {
        report.info("No logo ([menu] logo): the track select shows no name for the track.");
    }
    else {
        std::vector<std::array<uint8_t, 4>> px(size_t(logo_w * logo_h));
        for (size_t i = 0; i < px.size(); i++) px[i] = { rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3] };
        std::vector<uint8_t> indices;
        std::array<uint16_t, 256> palette{};
        rush2::track2049::quantize_ci8(px, indices, palette);
        for (int y = 0; y < logo_h; y++) {
            int stored = logo_h - 1 - y;
            memcpy(&art.logo[size_t(stored * logo_w)], &indices[size_t(y * logo_w)], logo_w);
            for (int x = 0; x < logo_w; x++) {
                const uint8_t* p = &rgba[size_t((y * logo_w + x) * 4)];
                uint8_t* q = &art.logo_rgba[size_t((stored * logo_w + x) * 4)];
                if (p[3] >= 128) { q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 0xFF; }
            }
        }
        for (int i = 0; i < 256; i++) {
            art.logo_palette[size_t(i * 2)] = uint8_t(palette[size_t(i)] >> 8);
            art.logo_palette[size_t(i * 2 + 1)] = uint8_t(palette[size_t(i)]);
        }
    }

    // Diorama: the author's (converted with the track), else Rush 2049's preview of the route (race track 4's red).
    art.route_preview = false;
    if (art.diorama.empty()) {
        Bytes picture = fit_picture(extras.preview, picture_size, picture_size, "preview", report);
        art.route_preview = rush2::track2049::route_preview(track.track.path, 4, picture, art.diorama, art.diorama_list,
                                                            art.diorama_pointers, art.diorama_radius);
        if (!art.route_preview) {
            art.diorama.clear();
            report.warning("The track select preview of the AI path couldn't be made; it shows no diorama.");
        }
    }
}

// Cache file: "MART", version, then each field as a u32 length (in bytes or entries) and its data.
std::vector<uint8_t> rush2::track_mod::serialize(const MenuArt& art) {
    Bytes out = { 'M', 'A', 'R', 'T' };
    add32(out, 3);
    for (const Bytes* b : { &art.logo, &art.logo_palette, &art.logo_rgba, &art.diorama }) {
        add32(out, uint32_t(b->size()));
        out.insert(out.end(), b->begin(), b->end());
    }
    add32(out, art.diorama_list);
    add32(out, uint32_t(art.diorama_pointers.size()));
    for (uint32_t p : art.diorama_pointers) add32(out, p);
    uint32_t r;
    memcpy(&r, &art.diorama_radius, 4);
    add32(out, r);
    add32(out, art.route_preview ? 1 : 0);
    return out;
}

bool rush2::track_mod::deserialize(const std::vector<uint8_t>& in, MenuArt& art) {
    size_t at = 4;
    auto u32 = [&](uint32_t& v) {
        if (at + 4 > in.size()) return false;
        v = get32(in, at);
        at += 4;
        return true;
    };
    uint32_t version;
    if (in.size() < 8 || memcmp(in.data(), "MART", 4) != 0 || !u32(version) || version != 3) return false;
    for (Bytes* b : { &art.logo, &art.logo_palette, &art.logo_rgba, &art.diorama }) {
        uint32_t n;
        if (!u32(n) || at + n > in.size()) return false;
        b->assign(in.begin() + at, in.begin() + at + n);
        at += n;
    }
    uint32_t n, r, preview;
    if (!u32(art.diorama_list) || !u32(n)) return false;
    art.diorama_pointers.clear();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p;
        if (!u32(p)) return false;
        art.diorama_pointers.push_back(p);
    }
    if (!u32(r) || !u32(preview)) return false;
    memcpy(&art.diorama_radius, &r, 4);
    art.route_preview = preview != 0;
    return art.logo.size() == size_t(logo_w * logo_h) && art.logo_palette.size() == 512;
}
