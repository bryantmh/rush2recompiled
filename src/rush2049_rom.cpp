// Reads any file out of the Rush 2049 (USA) ROM.
//
// Rush 2049 keeps a sorted table of ROM offsets for its 183 files at 0x8011B5BC in its main code segment, which is
// stored raw-deflate compressed at ROM 0xB0CB10 and loaded at 0x80086A50. A file's data runs up to the next file's
// offset; the main code follows the last file. Files are raw, raw-deflate compressed or LZ compressed
// (rush2::wings::lz_decompress); which one isn't recorded anywhere, so a file is taken as deflate if it inflates to
// the end of its data, else as LZ.

#include <cstdio>
#include <mutex>

#include "assets.h"
#include "rush2049_rom.h"
#include "wings.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t boot_rom = 0x1000;
    constexpr uint32_t boot_size = 0x86650; // Up to the main segment's address.
    constexpr uint32_t main_rom = 0xB0CB10;
    constexpr uint32_t battle_rom = 0xB6FEC4;
    constexpr uint32_t file_table_vram = 0x8011B5BC;

    uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
        return (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
    }

    // The file table of rom, file_count + 1 entries (the last one the main code's offset).
    bool read_table(const std::vector<uint8_t>& rom, std::vector<uint32_t>& offsets) {
        offsets.clear();
        std::vector<uint8_t> main_code;
        if (rom.size() <= main_rom || !rush2::assets::inflate_raw(rom.data() + main_rom, rom.size() - main_rom, main_code)) {
            return false;
        }
        size_t pos = file_table_vram - rush2::rom2049::main_vram;
        while (pos + 4 <= main_code.size()) {
            uint32_t offset = be32(main_code, pos);
            if (!offsets.empty() && (offset <= offsets.back() || offset >= main_rom)) {
                break;
            }
            offsets.push_back(offset);
            pos += 4;
        }
        if (offsets.size() != rush2::rom2049::file_count) {
            printf("[2049] Unexpected file table (%zu files)\n", offsets.size());
            offsets.clear();
            return false;
        }
        offsets.push_back(main_rom);
        return true;
    }

    bool decompress(const std::vector<uint8_t>& rom, int index, uint32_t start, uint32_t end, std::vector<uint8_t>& out) {
        // Inflating only succeeds on a complete deflate stream, which LZ data practically never forms. The sound banks
        // (6-8) and songs (10-21) are known LZ files, and inflating file 7 never returns, so those skip it. File 9, the
        // sample data, is stored raw.
        if (index == 9) {
            out.assign(rom.begin() + start, rom.begin() + end);
            return true;
        }
        bool known_lz = (index >= 6 && index <= 8) || (index >= 10 && index <= 21);
        if (!known_lz && rush2::assets::inflate_raw(rom.data() + start, end - start, out) && !out.empty()) {
            return true;
        }
        if (rush2::wings::lz_decompress(rom.data() + start, end - start, out)) {
            return true;
        }
        printf("[2049] Failed to decompress file %d\n", index);
        return false;
    }

    class N64Source : public rush2::rom2049::Source {
    public:
        explicit N64Source(std::shared_ptr<const std::vector<uint8_t>> rom) : rom(std::move(rom)) {}

        bool read_file(int index, std::vector<uint8_t>& out) const override {
            uint32_t start, end;
            {
                std::lock_guard lock{ mutex };
                if (offsets.empty() && !read_table(*rom, offsets)) {
                    return false;
                }
                if (index < 0 || index >= rush2::rom2049::file_count) {
                    return false;
                }
                start = offsets[index];
                end = offsets[index + 1];
            }
            return decompress(*rom, index, start, end, out);
        }

        std::shared_ptr<const std::vector<uint8_t>> segment(rush2::rom2049::Segment s) const override {
            std::lock_guard lock{ mutex };
            auto& cached = segments[(int)s];
            if (cached == nullptr) {
                auto data = std::make_shared<std::vector<uint8_t>>();
                switch (s) {
                    case rush2::rom2049::Segment::Boot:
                        if (rom->size() >= boot_rom + boot_size) {
                            data->assign(rom->begin() + boot_rom, rom->begin() + boot_rom + boot_size);
                        }
                        break;
                    case rush2::rom2049::Segment::Main:
                        if (rom->size() > main_rom) {
                            rush2::assets::inflate_raw(rom->data() + main_rom, rom->size() - main_rom, *data);
                        }
                        break;
                    case rush2::rom2049::Segment::Battle:
                        if (rom->size() > battle_rom) {
                            rush2::assets::inflate_raw(rom->data() + battle_rom, rom->size() - battle_rom, *data);
                        }
                        break;
                }
                if (data->empty()) {
                    return nullptr;
                }
                cached = std::move(data);
            }
            return cached;
        }

        const std::vector<uint8_t>* n64_rom() const override {
            return rom.get();
        }

    private:
        std::shared_ptr<const std::vector<uint8_t>> rom;
        mutable std::mutex mutex;
        mutable std::vector<uint32_t> offsets; // file_count + 1 entries once read.
        mutable std::shared_ptr<const std::vector<uint8_t>> segments[3];
    };
}

uint32_t rush2::rom2049::segment_vram(Segment s) {
    switch (s) {
        case Segment::Boot: return boot_vram;
        case Segment::Main: return main_vram;
        case Segment::Battle: return battle_vram;
    }
    return 0;
}

std::shared_ptr<const rush2::rom2049::Source> rush2::rom2049::n64_source(std::shared_ptr<const std::vector<uint8_t>> rom) {
    return std::make_shared<N64Source>(std::move(rom));
}

bool rush2::rom2049::read_file(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out) {
    std::vector<uint32_t> offsets;
    if (!read_table(rom, offsets) || index < 0 || index >= file_count) {
        return false;
    }
    return decompress(rom, index, offsets[index], offsets[index + 1], out);
}

bool rush2::rom2049::list_image(const std::vector<uint8_t>& file, uint32_t imag, uint32_t record,
                                std::vector<std::array<uint8_t, 4>>& rgba, int& w, int& h) {
    auto be32 = [&](size_t o) {
        return (uint32_t(file[o]) << 24) | (uint32_t(file[o + 1]) << 16) | (uint32_t(file[o + 2]) << 8) | file[o + 3];
    };
    if ((uint64_t)record + 0x24 > file.size() || (be32(record + 28) & 0x08000000)) return false;
    size_t list = (size_t)imag + be32(record + 24);
    uint32_t texels = UINT32_MAX, fmt = 0, siz = 0;
    w = h = 0;
    for (size_t o = list; o + 8 <= file.size() && file[o] != 0xDF; o += 8) {
        uint32_t w0 = be32(o), w1 = be32(o + 4);
        if (file[o] == 0xFD && texels == UINT32_MAX) texels = w1 & 0xFFFFFF;
        if (file[o] == 0xF5 && ((w1 >> 24) & 7) == 0) {
            fmt = (w0 >> 21) & 7;
            siz = (w0 >> 19) & 3;
        }
        if (file[o] == 0xF2 && ((w1 >> 24) & 7) == 0) {
            w = (int)((w1 >> 14) & 0x3FF) + 1;
            h = (int)((w1 >> 2) & 0x3FF) + 1;
        }
    }
    if (texels == UINT32_MAX || fmt != 0 || (siz != 2 && siz != 3) || w <= 0 || h <= 0) return false;
    size_t at = (size_t)imag + texels, bytes = siz == 3 ? 4 : 2;
    if (at + (size_t)w * h * bytes > file.size()) return false;
    rgba.resize((size_t)w * h);
    for (size_t i = 0; i < rgba.size(); i++) {
        const uint8_t* p = &file[at + i * bytes];
        if (siz == 3) {
            rgba[i] = { p[0], p[1], p[2], p[3] };
        }
        else {
            uint16_t v = uint16_t(p[0] << 8 | p[1]);
            rgba[i] = { uint8_t((v >> 11) << 3), uint8_t(((v >> 6) & 31) << 3), uint8_t(((v >> 1) & 31) << 3), uint8_t(v & 1 ? 255 : 0) };
        }
    }
    return true;
}
