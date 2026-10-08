// Image work for texture upscaling (include/texture_upscale.h, docs/texture_upscaling.md): decoding textures from
// TMEM, padding and unpadding them around an upscaler, DDS files with mipmaps, texture pack files and running the
// upscaler. Nothing here touches the renderer or the UI, so it builds on its own for testing.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

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
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

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

    // sampleTMEM in TextureDecoder.hlsli.
    Rgba sample(const uint8_t* tmem, uint32_t x, uint32_t y, uint32_t fmt, uint32_t siz, uint32_t address,
                uint32_t stride, uint32_t tlut, uint32_t palette) {
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
                  uint32_t palette, uint32_t tlut, uint32_t width, uint32_t height) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(size_t(width) * height * 4);
    const uint32_t address = tmem << 3;
    const uint32_t stride = line << 3;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            Rgba c = sample(tmem_bytes, x, y, fmt, siz, address, stride, tlut, palette);
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

bool write_png(const std::filesystem::path& path, const Image& image) {
    int length = 0;
    unsigned char* png = stbi_write_png_to_mem(image.rgba.data(), (int)image.width * 4, (int)image.width,
                                               (int)image.height, 4, &length);
    if (png == nullptr) {
        return false;
    }
    bool ok = write_file(path, std::vector<uint8_t>(png, png + length));
    STBIW_FREE(png);
    return ok;
}

int run_command(const std::string& command_line) {
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
    WaitForSingleObject(process.hProcess, INFINITE);
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
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& folder) {
    std::vector<uint8_t> bytes = read_file(zip);
    mz_zip_archive archive{};
    if (bytes.empty() || !mz_zip_reader_init_mem(&archive, bytes.data(), bytes.size(), 0)) {
        return false;
    }
    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count && ok; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            ok = false;
            break;
        }
        std::filesystem::path relative = std::filesystem::path(stat.m_filename).lexically_normal();
        if (relative.is_absolute() || (!relative.empty() && *relative.begin() == "..")) {
            continue; // Never write outside the folder.
        }
        std::filesystem::path target = folder / relative;
        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&archive, i)) {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        if (data == nullptr) {
            ok = false;
            break;
        }
        ok = write_file(target, std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + size));
        mz_free(data);
    }
    mz_zip_reader_end(&archive);
    return ok;
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

bool zip_folder(const std::filesystem::path& folder, const std::filesystem::path& zip) {
    std::filesystem::path temp = zip;
    temp += ".part";
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_file(&archive, temp.string().c_str(), 0)) {
        return false;
    }
    bool ok = true;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); ok && !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        std::string name = std::filesystem::relative(it->path(), folder).generic_string();
        std::vector<uint8_t> bytes = read_file(it->path());
        // Images are compressed already or compress well enough at a fast level.
        ok = mz_zip_writer_add_mem(&archive, name.c_str(), bytes.data(), bytes.size(), MZ_BEST_SPEED) != 0;
    }
    ok = ok && !ec && mz_zip_writer_finalize_archive(&archive);
    mz_zip_writer_end(&archive);
    if (!ok) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::filesystem::rename(temp, zip, ec);
    return !ec;
}

}
