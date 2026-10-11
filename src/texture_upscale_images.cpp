// Image work for texture upscaling (include/texture_upscale.h, docs/texture_upscaling.md): decoding textures from
// TMEM, recoloring palette variants, padding and unpadding them around an upscaler, hqx, DDS files with mipmaps,
// texture pack files and running a custom upscaler. Nothing here touches the renderer or the UI, so it builds on its
// own for testing.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include "ddspp/ddspp.h"
#include "miniz.h" // librecomp's copy, which it links for mod files.
#include "stb/stb_image.h" // The implementation is in RT64's texture cache.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "hqx.h"
#include "texture_upscale.h"

namespace rush2::upscale {

namespace {
    // RDP constants (RT64's shared/rt64_f3d_defines.h).
    constexpr uint32_t fmt_rgba = 0, fmt_ci = 2, fmt_ia = 3, fmt_i = 4;
    constexpr uint32_t siz_4b = 0, siz_8b = 1, siz_16b = 2, siz_32b = 3;
    constexpr uint32_t tt_rgba16 = 2u << 14, tt_ia16 = 3u << 14;
    constexpr uint32_t tmem_palette = 0x800;
    constexpr uint32_t tmem_mask8 = 0xFFF, tmem_mask16 = 0x7FF;

    struct Rgba { uint8_t r, g, b, a; };

    Rgba gray(uint32_t i, uint32_t a) { return { (uint8_t)i, (uint8_t)i, (uint8_t)i, (uint8_t)a }; }

    Rgba rgba16(uint32_t v) {
        uint32_t r = (v >> 11) & 0x1F, g = (v >> 6) & 0x1F, b = (v >> 1) & 0x1F;
        return { (uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 3) | (g >> 2)), (uint8_t)((b << 3) | (b >> 2)),
                 (uint8_t)((v & 1) ? 255 : 0) };
    }

    Rgba ia16(uint32_t v) { return gray((v >> 8) & 0xFF, v & 0xFF); }

    // implLoadTMEM: odd rows of a texture have their 32-bit words swapped in pairs.
    uint32_t load_tmem(const uint8_t* tmem, uint32_t relative, uint32_t mask, uint32_t or_address, bool odd_row,
                       uint32_t start, uint32_t row_size) {
        uint32_t final_address = start + relative;
        if (odd_row && row_size > 0) {
            uint32_t row_start = (relative / row_size) * row_size;
            uint32_t word = (relative - row_start) / 4;
            final_address = start + row_start + ((word ^ 1) * 4) + (relative & 3);
        }
        return tmem[((final_address & mask) | or_address) & tmem_mask8];
    }

    // sampleTMEM in TextureDecoder.hlsli. index gets the texel's palette index (the 4 or 8 bits it holds) when a TLUT
    // is used.
    Rgba sample(const uint8_t* tmem, uint32_t x, uint32_t y, uint32_t fmt, uint32_t siz, uint32_t address,
                uint32_t stride, uint32_t tlut, uint32_t palette, uint8_t& index) {
        const bool odd_row = (y & 1) != 0;
        const bool odd_column = (x & 1) != 0;
        const bool is_rgba32 = fmt == fmt_rgba && siz == siz_32b;
        const bool uses_tlut = tlut > 0;
        const uint32_t shift = is_rgba32 ? 2 : siz;
        const uint32_t mask = (is_rgba32 || uses_tlut) ? tmem_mask16 : tmem_mask8;
        auto load = [&](uint32_t relative, uint32_t or_address) {
            return load_tmem(tmem, relative, mask, or_address, odd_row, address, stride);
        };

        const uint32_t pixel_address = y * stride + ((x << shift) >> 1);
        const uint32_t p0 = load(pixel_address, 0);
        const uint32_t p1 = load(pixel_address + 1, 0);
        const uint32_t p4 = (p0 >> (odd_column ? 0 : 4)) & 0xF;

        if (uses_tlut) {
            index = uint8_t(siz == siz_4b ? p4 : p0);
            uint32_t entry = (siz == siz_4b) ? tmem_palette + (palette << 7) + (p4 << 3) : tmem_palette + (p0 << 3);
            uint32_t value = tmem[(entry + 1) & tmem_mask8] | (tmem[entry & tmem_mask8] << 8);
            switch (tlut) {
                case tt_rgba16: return rgba16(value);
                case tt_ia16: return ia16(value);
                default: return { 0, 0, 0, 255 };
            }
        }

        const uint32_t address2 = is_rgba32 ? pixel_address : pixel_address + 2;
        const uint32_t or_address = is_rgba32 ? 0x800 : 0;
        const uint32_t p2 = load(address2, or_address);
        const uint32_t p3 = load(address2 + 1, or_address);

        switch (siz) {
            case siz_4b:
                switch (fmt) {
                    case fmt_ci: { uint32_t i = (palette << 4) | p4; return gray(i & 0xFF, i & 0xFF); }
                    case fmt_ia: {
                        uint32_t i = p4 & 0xE;
                        i = (i << 4) | (i << 1) | (i >> 2);
                        return gray(i, (p4 & 1) ? 255 : 0);
                    }
                    case fmt_i:
                    case fmt_rgba: { uint32_t i = (p4 << 4) | p4; return gray(i, i); }
                    default: return { 0, 0, 0, 255 };
                }
            case siz_8b:
                switch (fmt) {
                    case fmt_ia: {
                        uint32_t i = (p0 >> 4) & 0xF, a = p0 & 0xF;
                        return gray((i << 4) | i, (a << 4) | a);
                    }
                    case fmt_i:
                    case fmt_rgba:
                    case fmt_ci: return gray(p0, p0);
                    default: return { 0, 0, 0, 255 };
                }
            case siz_16b: {
                uint32_t value = p1 | (p0 << 8);
                switch (fmt) {
                    case fmt_rgba: return rgba16(value);
                    case fmt_ia: return ia16(value);
                    case fmt_ci:
                    case fmt_i: return { (uint8_t)p0, (uint8_t)p1, (uint8_t)p0, (uint8_t)p1 };
                    default: return { 0, 0, 0, 255 };
                }
            }
            case siz_32b:
                switch (fmt) {
                    case fmt_rgba: return { (uint8_t)p0, (uint8_t)p1, (uint8_t)p2, (uint8_t)p3 };
                    case fmt_ci:
                    case fmt_ia:
                    case fmt_i:
                        return odd_column ? Rgba{ (uint8_t)p0, (uint8_t)p1, (uint8_t)p0, (uint8_t)p1 }
                                          : Rgba{ (uint8_t)p2, (uint8_t)p3, (uint8_t)p2, (uint8_t)p3 };
                    default: return { 0, 0, 0, 255 };
                }
            default:
                return { 0, 0, 0, 255 };
        }
    }

    // Index of the source texel for a coordinate past the edge.
    int32_t edge_index(int32_t i, int32_t size, Edge edge) {
        switch (edge) {
            case Edge::Clamp:
                return std::clamp(i, 0, size - 1);
            case Edge::Mirror: {
                int32_t period = size * 2;
                int32_t m = ((i % period) + period) % period;
                return m < size ? m : period - 1 - m;
            }
            case Edge::Wrap:
            default:
                return ((i % size) + size) % size;
        }
    }

    // One level down: each texel averages the 2x2 block (or 2x1 at a side of 1) it covers, color weighted by alpha
    // (TextureDecodeCS.hlsl does the same for the mipmaps RT64 generates).
    Image half_size(const Image& image) {
        Image out;
        out.width = std::max(image.width / 2, 1u);
        out.height = std::max(image.height / 2, 1u);
        out.rgba.resize(size_t(out.width) * out.height * 4);
        const uint32_t bw = image.width > 1 ? 2 : 1;
        const uint32_t bh = image.height > 1 ? 2 : 1;
        for (uint32_t y = 0; y < out.height; y++) {
            for (uint32_t x = 0; x < out.width; x++) {
                float weighted[3] = {}, plain[3] = {}, alpha = 0.0f;
                for (uint32_t dy = 0; dy < bh; dy++) {
                    for (uint32_t dx = 0; dx < bw; dx++) {
                        const uint8_t* p = &image.rgba[(size_t(y * bh + dy) * image.width + (x * bw + dx)) * 4];
                        float a = p[3] / 255.0f;
                        for (int c = 0; c < 3; c++) {
                            weighted[c] += p[c] * a;
                            plain[c] += p[c];
                        }
                        alpha += a;
                    }
                }
                const float count = float(bw * bh);
                uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
                for (int c = 0; c < 3; c++) {
                    float v = alpha > 0.0f ? weighted[c] / alpha : plain[c] / count;
                    o[c] = (uint8_t)std::clamp(int(v + 0.5f), 0, 255);
                }
                o[3] = (uint8_t)std::clamp(int(alpha / count * 255.0f + 0.5f), 0, 255);
            }
        }
        return out;
    }

    void append_json_string(std::string& out, const std::string& text) {
        out += '"';
        for (char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        out += '"';
    }
}

Image decode_tmem(const uint8_t* tmem_bytes, uint32_t fmt, uint32_t siz, uint32_t tmem, uint32_t line,
                  uint32_t palette, uint32_t tlut, uint32_t width, uint32_t height, std::vector<uint8_t>* indices) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(size_t(width) * height * 4);
    if (indices != nullptr) {
        indices->assign(size_t(width) * height, 0);
    }
    const uint32_t address = tmem << 3;
    const uint32_t stride = line << 3;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t index = 0;
            Rgba c = sample(tmem_bytes, x, y, fmt, siz, address, stride, tlut, palette, index);
            if (indices != nullptr) {
                (*indices)[size_t(y) * width + x] = index;
            }
            uint8_t* p = &image.rgba[(size_t(y) * width + x) * 4];
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = c.a;
        }
    }
    return image;
}

uint64_t content_key(const Image& image) {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    XXH3_64bits_update(&state, &image.width, sizeof(image.width));
    XXH3_64bits_update(&state, &image.height, sizeof(image.height));
    XXH3_64bits_update(&state, image.rgba.data(), image.rgba.size());
    return XXH3_64bits_digest(&state);
}

std::string key_name(uint64_t key) {
    char text[17];
    snprintf(text, sizeof(text), "%016llx", (unsigned long long)key);
    return text;
}

bool parse_key_name(const std::string& name, uint64_t& key) {
    if (name.size() < 16) {
        return false;
    }
    key = 0;
    for (size_t i = 0; i < 16; i++) {
        char c = name[i];
        uint64_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        key = (key << 4) | digit;
    }
    return true;
}

bool worth_upscaling(const Image& image) {
    if (image.width < 4 || image.height < 4 || image.width * image.height < 64) {
        return false;
    }
    for (size_t i = 4; i < image.rgba.size(); i += 4) {
        if (memcmp(&image.rgba[i], &image.rgba[0], 4) != 0) {
            return true;
        }
    }
    return false;
}

Image pad(const Image& image, uint32_t border, Edge edge_s, Edge edge_t) {
    Image out;
    out.width = image.width + border * 2;
    out.height = image.height + border * 2;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; y++) {
        int32_t sy = edge_index(int32_t(y) - int32_t(border), int32_t(image.height), edge_t);
        for (uint32_t x = 0; x < out.width; x++) {
            int32_t sx = edge_index(int32_t(x) - int32_t(border), int32_t(image.width), edge_s);
            memcpy(&out.rgba[(size_t(y) * out.width + x) * 4], &image.rgba[(size_t(sy) * image.width + sx) * 4], 4);
        }
    }
    return out;
}

Image crop(const Image& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Image out;
    if (x + width > image.width || y + height > image.height) {
        return out;
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(size_t(width) * height * 4);
    for (uint32_t row = 0; row < height; row++) {
        memcpy(&out.rgba[size_t(row) * width * 4], &image.rgba[(size_t(y + row) * image.width + x) * 4], size_t(width) * 4);
    }
    return out;
}

Image resize_bilinear(const Image& image, uint32_t width, uint32_t height) {
    Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++) {
        float fy = std::clamp((y + 0.5f) * image.height / height - 0.5f, 0.0f, float(image.height - 1));
        uint32_t y0 = uint32_t(fy), y1 = std::min(y0 + 1, image.height - 1);
        float ty = fy - y0;
        for (uint32_t x = 0; x < width; x++) {
            float fx = std::clamp((x + 0.5f) * image.width / width - 0.5f, 0.0f, float(image.width - 1));
            uint32_t x0 = uint32_t(fx), x1 = std::min(x0 + 1, image.width - 1);
            float tx = fx - x0;
            for (int c = 0; c < 4; c++) {
                auto at = [&](uint32_t px, uint32_t py) { return float(image.rgba[(size_t(py) * image.width + px) * 4 + c]); };
                float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out.rgba[(size_t(y) * width + x) * 4 + c] = (uint8_t)std::clamp(int(top + (bottom - top) * ty + 0.5f), 0, 255);
            }
        }
    }
    return out;
}

void restore_alpha(Image& upscaled, const Image& original) {
    bool original_opaque = true, original_cutout = true;
    for (size_t i = 3; i < original.rgba.size(); i += 4) {
        uint8_t a = original.rgba[i];
        original_opaque &= a == 255;
        original_cutout &= a == 0 || a == 255;
    }
    if (original_opaque) {
        for (size_t i = 3; i < upscaled.rgba.size(); i += 4) {
            upscaled.rgba[i] = 255;
        }
        return;
    }

    bool upscaled_opaque = true;
    for (size_t i = 3; i < upscaled.rgba.size() && upscaled_opaque; i += 4) {
        upscaled_opaque &= upscaled.rgba[i] == 255;
    }
    if (upscaled_opaque) {
        // The upscaler dropped alpha (or made it opaque): scale the original's up.
        Image alpha = resize_bilinear(original, upscaled.width, upscaled.height);
        for (size_t i = 3; i < upscaled.rgba.size(); i += 4) {
            upscaled.rgba[i] = alpha.rgba[i];
        }
    }
    if (original_cutout) {
        for (size_t i = 3; i < upscaled.rgba.size(); i += 4) {
            upscaled.rgba[i] = upscaled.rgba[i] >= 128 ? 255 : 0;
        }
    }
}

Image unpad(const Image& output, const Image& original, uint32_t border) {
    const uint32_t padded_width = original.width + border * 2;
    const uint32_t padded_height = original.height + border * 2;
    if (border > 0 && output.width % padded_width == 0 && output.height % padded_height == 0 &&
        output.width / padded_width == output.height / padded_height && output.width >= padded_width) {
        uint32_t scale = output.width / padded_width;
        return crop(output, border * scale, border * scale, original.width * scale, original.height * scale);
    }
    if (output.width % original.width == 0 && output.height % original.height == 0 &&
        output.width / original.width == output.height / original.height && output.width >= original.width) {
        return output;
    }
    return {};
}

std::vector<uint8_t> make_dds(const Image& image) {
    std::vector<Image> levels{ image };
    while (levels.back().width > 1 || levels.back().height > 1) {
        levels.push_back(half_size(levels.back()));
    }

    ddspp::Header header{};
    ddspp::HeaderDXT10 header10{};
    ddspp::encode_header(ddspp::R8G8B8A8_UNORM, image.width, image.height, 1, ddspp::Texture2D,
                         (unsigned int)levels.size(), 1, header, header10);

    std::vector<uint8_t> bytes(sizeof(ddspp::DDS_MAGIC) + sizeof(header) + sizeof(header10));
    memcpy(bytes.data(), &ddspp::DDS_MAGIC, sizeof(ddspp::DDS_MAGIC));
    memcpy(bytes.data() + sizeof(ddspp::DDS_MAGIC), &header, sizeof(header));
    memcpy(bytes.data() + sizeof(ddspp::DDS_MAGIC) + sizeof(header), &header10, sizeof(header10));
    for (const Image& level : levels) {
        bytes.insert(bytes.end(), level.rgba.begin(), level.rgba.end());
    }
    return bytes;
}

Image downscale(const Image& image, uint32_t factor) {
    Image out = image;
    for (; factor > 1; factor /= 2) {
        out = half_size(out);
    }
    return out;
}

uint64_t index_key(const std::vector<uint8_t>& indices, uint32_t width, uint32_t height, uint32_t siz) {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    uint32_t header[3] = { width, height, siz };
    XXH3_64bits_update(&state, header, sizeof(header));
    XXH3_64bits_update(&state, indices.data(), indices.size());
    return XXH3_64bits_digest(&state) | 1; // Never 0, which means "not paletted".
}

Image hq_upscale(const Image& image, uint32_t scale) {
    // hqx works on 0xAARRGGBB words.
    std::vector<uint32_t> src(size_t(image.width) * image.height);
    for (size_t i = 0; i < src.size(); i++) {
        const uint8_t* p = &image.rgba[i * 4];
        src[i] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
    Image out;
    out.width = image.width * scale;
    out.height = image.height * scale;
    std::vector<uint32_t> dst(size_t(out.width) * out.height);
    if (scale == 4) {
        hq4x_32(src.data(), dst.data(), (int)image.width, (int)image.height);
    }
    else {
        hq2x_32(src.data(), dst.data(), (int)image.width, (int)image.height);
    }
    out.rgba.resize(dst.size() * 4);
    for (size_t i = 0; i < dst.size(); i++) {
        uint32_t c = dst[i];
        out.rgba[i * 4 + 0] = uint8_t(c >> 16);
        out.rgba[i * 4 + 1] = uint8_t(c >> 8);
        out.rgba[i * 4 + 2] = uint8_t(c);
        out.rgba[i * 4 + 3] = uint8_t(c >> 24);
    }
    return out;
}

bool matches_original(const Image& upscaled, const Image& original) {
    if (upscaled.empty() || upscaled.width % original.width != 0 || upscaled.height % original.height != 0) {
        return false;
    }
    // Averaged back down to the original's size, a real upscale is close to it; garbage isn't.
    Image down = upscaled;
    while (down.width > original.width || down.height > original.height) {
        down = half_size(down);
    }
    if (down.width != original.width || down.height != original.height) {
        down = resize_bilinear(upscaled, original.width, original.height);
    }
    uint64_t total = 0, count = 0;
    for (size_t i = 0; i < original.rgba.size(); i += 4) {
        if (original.rgba[i + 3] < 128) {
            continue; // Transparent texels' colors are anyone's guess.
        }
        for (int c = 0; c < 3; c++) {
            total += (uint64_t)std::abs(int(down.rgba[i + c]) - int(original.rgba[i + c]));
        }
        count += 3;
    }
    return count == 0 || total <= count * 24;
}

RecolorPlan plan_recolor(const Image& upscaled, const Image& old_colors, Edge edge_s, Edge edge_t, unsigned threads) {
    // Each upscaled pixel is explained as a blend of two of the original texels around it (or one): the pair whose
    // old colors, mixed, come closest to the pixel, with a small preference for nearby texels. Which pair and how much
    // of each depend only on the upscale and the old colors, so the search is done once and apply_recolor reuses it
    // for every new palette (a car's paint changes on the car select are then a cheap mix, not this search again).
    RecolorPlan plan;
    plan.width = upscaled.width;
    plan.height = upscaled.height;
    plan.pixels.resize(size_t(upscaled.width) * upscaled.height);
    const float scale_x = float(upscaled.width) / float(old_colors.width);
    const float scale_y = float(upscaled.height) / float(old_colors.height);
    constexpr float spatial_cost = 60.0f; // Per squared texel of distance, against squared color distance.
    // Rows in bands, one per thread (a 4x panel's search is about 7 ms on one).
    auto rows = [&](uint32_t y_begin, uint32_t y_end) {
        for (uint32_t y = y_begin; y < y_end; y++) {
            float fy = (y + 0.5f) / scale_y - 0.5f;
            int32_t cy = (int32_t)std::lround(fy);
            for (uint32_t x = 0; x < upscaled.width; x++) {
                float fx = (x + 0.5f) / scale_x - 0.5f;
                int32_t cx = (int32_t)std::lround(fx);

                // The 3x3 texels around the nearest one.
                float old_color[9][4], spatial[9];
                for (int dy = -1; dy <= 1; dy++) {
                    int32_t sy = edge_index(cy + dy, int32_t(old_colors.height), edge_t);
                    for (int dx = -1; dx <= 1; dx++) {
                        int32_t sx = edge_index(cx + dx, int32_t(old_colors.width), edge_s);
                        const int c = (dy + 1) * 3 + dx + 1;
                        const uint8_t* o = &old_colors.rgba[(size_t(sy) * old_colors.width + sx) * 4];
                        for (int k = 0; k < 4; k++) old_color[c][k] = o[k];
                        float ddx = float(cx + dx) - fx, ddy = float(cy + dy) - fy;
                        spatial[c] = ddx * ddx + ddy * ddy;
                    }
                }

                const uint8_t* u = &upscaled.rgba[(size_t(y) * upscaled.width + x) * 4];
                float best_cost = 1e30f, best_t = 0.0f;
                int best_a = 0, best_b = 0;
                for (int a = 0; a < 9; a++) {
                    for (int b = a; b < 9; b++) {
                        // Projection of the pixel onto the segment between the two old colors.
                        float seg[4], rel[4], seg_len = 0.0f, dot = 0.0f;
                        for (int k = 0; k < 4; k++) {
                            seg[k] = old_color[b][k] - old_color[a][k];
                            rel[k] = float(u[k]) - old_color[a][k];
                            seg_len += seg[k] * seg[k];
                            dot += seg[k] * rel[k];
                        }
                        float t = seg_len > 0.0f ? std::clamp(dot / seg_len, 0.0f, 1.0f) : 0.0f;
                        float distance = 0.0f;
                        for (int k = 0; k < 4; k++) {
                            float d = rel[k] - seg[k] * t;
                            distance += d * d;
                        }
                        float cost = distance + spatial_cost * ((1.0f - t) * spatial[a] + t * spatial[b]);
                        if (cost < best_cost) {
                            best_cost = cost;
                            best_t = t;
                            best_a = a;
                            best_b = b;
                        }
                    }
                }
                RecolorPlan::Pixel& p = plan.pixels[size_t(y) * upscaled.width + x];
                p.cx = int16_t(cx);
                p.cy = int16_t(cy);
                p.a = uint8_t(best_a);
                p.b = uint8_t(best_b);
                p.t = best_t;
            }
        }
    };
    threads = std::clamp(threads, 1u, std::max(1u, upscaled.height / 16));
    std::vector<std::thread> helpers;
    for (unsigned t = 1; t < threads; t++) {
        helpers.emplace_back(rows, upscaled.height * t / threads, upscaled.height * (t + 1) / threads);
    }
    rows(0, upscaled.height / threads);
    for (std::thread& h : helpers) h.join();
    return plan;
}

Image apply_recolor(const RecolorPlan& plan, const Image& upscaled, const Image& old_colors, const Image& new_colors,
                    Edge edge_s, Edge edge_t, bool brightness_detail) {
    // The pixel becomes its plan's mix of the two texels' new colors, plus whatever detail the mix of their old colors
    // didn't explain, scaled by how much brighter or darker the new colors are. So a dark red pixel on the edge between
    // red paint and a black line turns dark blue when the paint is blue, instead of keeping a red fringe. A pixel none
    // of whose 3x3 texels changed stays as upscaled.
    Image out = upscaled;
    if (plan.width != upscaled.width || plan.height != upscaled.height) return out;
    for (uint32_t y = 0; y < upscaled.height; y++) {
        for (uint32_t x = 0; x < upscaled.width; x++) {
            const RecolorPlan::Pixel& p = plan.pixels[size_t(y) * upscaled.width + x];
            size_t at[9];
            bool changed = false;
            for (int dy = -1; dy <= 1; dy++) {
                int32_t sy = edge_index(p.cy + dy, int32_t(old_colors.height), edge_t);
                for (int dx = -1; dx <= 1; dx++) {
                    int32_t sx = edge_index(p.cx + dx, int32_t(old_colors.width), edge_s);
                    const int c = (dy + 1) * 3 + dx + 1;
                    at[c] = (size_t(sy) * old_colors.width + sx) * 4;
                    changed |= memcmp(&old_colors.rgba[at[c]], &new_colors.rgba[at[c]], 4) != 0;
                }
            }
            if (!changed) {
                continue;
            }

            const uint8_t* u = &upscaled.rgba[(size_t(y) * upscaled.width + x) * 4];
            const uint8_t* oa = &old_colors.rgba[at[p.a]];
            const uint8_t* ob = &old_colors.rgba[at[p.b]];
            const uint8_t* na = &new_colors.rgba[at[p.a]];
            const uint8_t* nb = &new_colors.rgba[at[p.b]];
            float old_mix[4], new_mix[4];
            for (int k = 0; k < 4; k++) {
                old_mix[k] = oa[k] + (float(ob[k]) - oa[k]) * p.t;
                new_mix[k] = na[k] + (float(nb[k]) - na[k]) * p.t;
            }
            float old_luma = 0.299f * old_mix[0] + 0.587f * old_mix[1] + 0.114f * old_mix[2];
            float new_luma = 0.299f * new_mix[0] + 0.587f * new_mix[1] + 0.114f * new_mix[2];
            float detail_scale = std::clamp((new_luma + 16.0f) / (old_luma + 16.0f), 0.0f, 4.0f);
            uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
            // brightness_detail: the detail's brightness only, the same on every channel. An upscaler drifts hues a
            // little where colors meet; added per channel, that drift tinted a white car's paint pink and green.
            const float detail_luma = 0.299f * u[0] + 0.587f * u[1] + 0.114f * u[2] - old_luma;
            for (int k = 0; k < 3; k++) {
                float detail = brightness_detail ? detail_luma : float(u[k]) - old_mix[k];
                float v = new_mix[k] + detail * detail_scale;
                o[k] = (uint8_t)std::clamp(int(v + 0.5f), 0, 255);
            }
            o[3] = (uint8_t)std::clamp(int(new_mix[3] + (float(u[3]) - old_mix[3]) + 0.5f), 0, 255);
        }
    }
    return out;
}

Image recolor(const Image& upscaled, const Image& old_colors, const Image& new_colors, Edge edge_s, Edge edge_t,
              bool brightness_detail) {
    return apply_recolor(plan_recolor(upscaled, old_colors, edge_s, edge_t), upscaled, old_colors, new_colors, edge_s,
                         edge_t, brightness_detail);
}

void clamp_chroma(Image& upscaled, const Image& colors, Edge edge_s, Edge edge_t) {
    // A pixel's color is its brightness plus a chroma (each channel minus the brightness). The chroma is held to the
    // range the 3x3 texels around the pixel have, a few levels either way; the brightness, and so the detail, stays.
    // A pixel over paint is a blend of its texels' colors with gray detail, so it is in range already: what this
    // catches is the color an upscaler spreads into small parts beside strong ones (green and pink headlights next to
    // violet paint).
    constexpr float margin = 3.0f;
    auto luma = [](const uint8_t* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    const float scale_x = float(upscaled.width) / float(colors.width);
    const float scale_y = float(upscaled.height) / float(colors.height);
    for (uint32_t y = 0; y < upscaled.height; y++) {
        int32_t cy = (int32_t)std::lround((y + 0.5f) / scale_y - 0.5f);
        for (uint32_t x = 0; x < upscaled.width; x++) {
            int32_t cx = (int32_t)std::lround((x + 0.5f) / scale_x - 0.5f);
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (int dy = -1; dy <= 1; dy++) {
                int32_t sy = edge_index(cy + dy, int32_t(colors.height), edge_t);
                for (int dx = -1; dx <= 1; dx++) {
                    int32_t sx = edge_index(cx + dx, int32_t(colors.width), edge_s);
                    const uint8_t* c = &colors.rgba[(size_t(sy) * colors.width + sx) * 4];
                    float l = luma(c);
                    for (int k = 0; k < 3; k++) {
                        lo[k] = std::min(lo[k], c[k] - l);
                        hi[k] = std::max(hi[k], c[k] - l);
                    }
                }
            }
            uint8_t* p = &upscaled.rgba[(size_t(y) * upscaled.width + x) * 4];
            float l = luma(p);
            for (int k = 0; k < 3; k++) {
                float chroma = std::clamp(p[k] - l, lo[k] - margin, hi[k] + margin);
                p[k] = (uint8_t)std::clamp(int(l + chroma + 0.5f), 0, 255);
            }
        }
    }
}

bool read_dds(const std::vector<uint8_t>& bytes, Image& image) {
    ddspp::Descriptor desc;
    if (bytes.size() < 148 || ddspp::decode_header((unsigned char*)bytes.data(), desc) != ddspp::Success ||
        desc.format != ddspp::R8G8B8A8_UNORM) {
        return false;
    }
    size_t size = size_t(desc.width) * desc.height * 4;
    if (bytes.size() < desc.headerSize + size) {
        return false;
    }
    image.width = desc.width;
    image.height = desc.height;
    image.rgba.assign(bytes.begin() + desc.headerSize, bytes.begin() + desc.headerSize + size);
    return true;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file{ path, std::ios::binary };
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    // Written under another name and renamed, so a file is never seen half written.
    std::filesystem::path temp = path;
    temp += ".part";
    {
        std::ofstream file{ temp, std::ios::binary };
        file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        if (!file) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

bool read_image(const std::filesystem::path& path, Image& image) {
    std::vector<uint8_t> bytes = read_file(path);
    if (bytes.empty()) {
        return false;
    }
    int width, height;
    stbi_uc* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &width, &height, nullptr, 4);
    if (data == nullptr) {
        return false;
    }
    image.width = (uint32_t)width;
    image.height = (uint32_t)height;
    image.rgba.assign(data, data + size_t(width) * height * 4);
    stbi_image_free(data);
    return true;
}

bool write_png_bytes(const Image& image, std::vector<uint8_t>& out) {
    int length = 0;
    unsigned char* png = stbi_write_png_to_mem(image.rgba.data(), (int)image.width * 4, (int)image.width,
                                               (int)image.height, 4, &length);
    if (png == nullptr) {
        return false;
    }
    out.assign(png, png + length);
    STBIW_FREE(png);
    return true;
}

bool write_png(const std::filesystem::path& path, const Image& image) {
    std::vector<uint8_t> png;
    return write_png_bytes(image, png) && write_file(path, png);
}

int run_command(const std::string& command_line, const std::atomic<bool>* cancel) {
#ifdef _WIN32
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, nullptr, 0);
    std::wstring wide(wide_length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, wide.data(), wide_length);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    while (WaitForSingleObject(process.hProcess, 50) == WAIT_TIMEOUT) {
        if (cancel != nullptr && cancel->load()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
#else
    const char* argv[] = { "nice", "-n", "10", "sh", "-c", command_line.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "nice", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        if (done < 0) {
            return -1;
        }
        if (cancel != nullptr && cancel->load()) {
            kill(pid, SIGKILL); // nice execs the shell, which runs the command in the same process.
            waitpid(pid, &status, 0);
            return -1;
        }
        usleep(50 * 1000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string path_utf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string pack_database(const std::vector<PackTexture>& textures) {
    // RT64's replacement database. Upscales line up with the originals' texels once shifted by half a texel, which
    // is how the console's bilinear filter places them (TextureSampler.hlsli).
    std::string out = "{\n    \"configuration\": { \"autoPath\": \"rt64\", \"configurationVersion\": 3, \"hashVersion\": 5 },\n"
                      "    \"textures\": [";
    bool first = true;
    for (const PackTexture& texture : textures) {
        std::string path = texture.path;
        size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            path.resize(dot); // RT64 adds the extension it finds.
        }
        for (uint64_t hash : texture.hashes) {
            out += first ? "\n        { \"path\": " : ",\n        { \"path\": ";
            first = false;
            append_json_string(out, path);
            out += ", \"hashes\": { \"rt64\": \"" + key_name(hash) + "\" }, \"shift\": \"half\" }";
        }
    }
    out += "\n    ]\n}\n";
    return out;
}

std::string pack_manifest(const std::string& id, const std::string& name, const std::string& description) {
    std::string out = "{\n    \"game_id\": \"rush2\",\n    \"id\": ";
    append_json_string(out, id);
    out += ",\n    \"display_name\": ";
    append_json_string(out, name);
    out += ",\n    \"description\": ";
    append_json_string(out, description);
    out += ",\n    \"short_description\": ";
    append_json_string(out, name);
    out += ",\n    \"version\": \"1.0.0\",\n    \"authors\": [\"You\"],\n    \"minimum_recomp_version\": \"0.0.0\"\n}\n";
    return out;
}

struct ZipWriter::Impl {
    mz_zip_archive archive{};
    bool ok = false;
    bool finished = false;
};

ZipWriter::ZipWriter() : impl(std::make_unique<Impl>()) {
    impl->ok = mz_zip_writer_init_heap(&impl->archive, 0, 0) != 0;
}

ZipWriter::~ZipWriter() {
    mz_zip_writer_end(&impl->archive);
}

bool ZipWriter::add(const std::string& name, const std::vector<uint8_t>& bytes) {
    // Images are compressed already or compress well enough at a fast level.
    impl->ok = impl->ok && !impl->finished &&
               mz_zip_writer_add_mem(&impl->archive, name.c_str(), bytes.data(), bytes.size(), MZ_BEST_SPEED) != 0;
    return impl->ok;
}

bool ZipWriter::write(const std::filesystem::path& zip) {
    void* data = nullptr;
    size_t size = 0;
    bool ok = impl->ok && !impl->finished && mz_zip_writer_finalize_heap_archive(&impl->archive, &data, &size);
    impl->finished = true;
    if (ok) {
        ok = write_file(zip, std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + size));
        mz_free(data); // Handed over by finalize; ending the writer doesn't free it.
    }
    return ok;
}

struct ZipReader::Impl {
    mz_zip_archive archive{};
    FILE* file = nullptr;
    bool ok = false;
};

ZipReader::ZipReader(const std::filesystem::path& zip) : impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl->file = _wfopen(zip.wstring().c_str(), L"rb");
#else
    impl->file = fopen(zip.c_str(), "rb");
#endif
    impl->ok = impl->file != nullptr && mz_zip_reader_init_cfile(&impl->archive, impl->file, 0, 0) != 0;
}

ZipReader::~ZipReader() {
    if (impl->ok) mz_zip_reader_end(&impl->archive);
    if (impl->file != nullptr) fclose(impl->file);
}

bool ZipReader::is_open() const {
    return impl->ok;
}

bool ZipReader::read(const std::string& name, std::vector<uint8_t>& bytes) {
    if (!impl->ok) return false;
    int index = mz_zip_reader_locate_file(&impl->archive, name.c_str(), nullptr, 0);
    if (index < 0) return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&impl->archive, (mz_uint)index, &size, 0);
    if (data == nullptr) return false;
    bytes.assign((uint8_t*)data, (uint8_t*)data + size);
    mz_free(data);
    return true;
}

std::vector<std::string> ZipReader::names() {
    std::vector<std::string> out;
    if (!impl->ok) return out;
    mz_uint count = mz_zip_reader_get_num_files(&impl->archive);
    for (mz_uint i = 0; i < count; i++) {
        if (mz_zip_reader_is_file_a_directory(&impl->archive, i)) continue;
        char name[512];
        if (mz_zip_reader_get_filename(&impl->archive, i, name, sizeof(name)) > 0) out.push_back(name);
    }
    return out;
}

bool decode_image(const std::vector<uint8_t>& bytes, Image& image) {
    if (read_dds(bytes, image)) return true;
    int width, height;
    stbi_uc* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &width, &height, nullptr, 4);
    if (data == nullptr) return false;
    image.width = (uint32_t)width;
    image.height = (uint32_t)height;
    image.rgba.assign(data, data + size_t(width) * height * 4);
    stbi_image_free(data);
    return true;
}

bool zip_folder(const std::filesystem::path& folder, const std::filesystem::path& zip) {
    ZipWriter writer;
    bool ok = true;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); ok && !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        std::string name = path_utf8(std::filesystem::relative(it->path(), folder).generic_u8string());
        ok = writer.add(name, read_file(it->path()));
    }
    return ok && !ec && writer.write(zip);
}

}
