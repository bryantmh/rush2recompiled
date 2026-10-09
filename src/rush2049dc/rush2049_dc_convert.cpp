// N64 Rush 2049 files made from the Dreamcast disc's files. See docs/rush2049_research/dreamcast.md.
//
// The Dreamcast game reads the same data formats as the N64 one, written little endian, except for models (a
// different display format, src/rush2049dc/rush2049_dc_model.cpp). AI paths are even stored big endian, byte for byte the N64's.
// Placement and collision files are swapped here field by field (docs/rush2049_research/placement.md section 2,
// collision.md sections 2-3); names they hold go through the Dreamcast-to-N64 name map.

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "rush2049_dc_internal.h"

namespace {
    using Bytes = std::vector<uint8_t>;

    uint32_t le32(const Bytes& d, size_t o) {
        return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    }
    uint16_t le16(const Bytes& d, size_t o) {
        return uint16_t(d[o] | (d[o + 1] << 8));
    }
    // In-place swaps of a field, in a copy that starts out as the little-endian file.
    void swap32(Bytes& d, size_t o) {
        std::swap(d[o], d[o + 3]);
        std::swap(d[o + 1], d[o + 2]);
    }
    void swap16(Bytes& d, size_t o) {
        std::swap(d[o], d[o + 1]);
    }
    void swap32s(Bytes& d, size_t o, int n) {
        for (int i = 0; i < n; i++) swap32(d, o + i * 4);
    }
    void swap16s(Bytes& d, size_t o, int n) {
        for (int i = 0; i < n; i++) swap16(d, o + i * 2);
    }

    // Race track k (1-6) is TRACKk, battle arena DMk (1-8), stunt arena STUNTk (1-4), then the obstacle course: the
    // N64's numbering of track geometry (100 + id), placement (119 + id), collision (138 + id) and AI paths
    // (157 + id, backward 176 + id).
    std::string track_base(int id) {
        if (id >= 1 && id <= 6) return "TRACK" + std::to_string(id);
        if (id >= 7 && id <= 14) return "DM" + std::to_string(id - 6);
        if (id >= 15 && id <= 18) return "STUNT" + std::to_string(id - 14);
        if (id == 19) return "OBSTACLE1";
        return "";
    }

    // Swaps the chunk directory of a tagged file (u32 directory offset, u32 count, then {tag, u32, u32} at the
    // directory) and returns {tag: (offset, count)}; tags come out in the N64's byte order.
    struct Chunk {
        uint32_t offset = 0, count = 0;
        bool found = false;
    };
    bool swap_directory(Bytes& d, std::map<std::string, Chunk>& out) {
        if (d.size() < 8) return false;
        uint32_t dir = le32(d, 0), n = le32(d, 4);
        if ((uint64_t)dir + (uint64_t)n * 12 > d.size()) return false;
        swap32s(d, 0, 2);
        for (uint32_t i = 0; i < n; i++) {
            size_t e = dir + i * 12;
            std::reverse(d.begin() + e, d.begin() + e + 4);
            std::string tag((const char*)&d[e], 4);
            Chunk c{ le32(d, e + 4), le32(d, e + 8), true };
            swap32s(d, e + 4, 2);
            out[tag] = c;
        }
        return true;
    }

    // Name fields: renamed through the name map, NUL padded to their size.
    void rename_field(Bytes& d, size_t o, size_t size) {
        std::string name((const char*)&d[o], strnlen((const char*)&d[o], size));
        std::string mapped = rush2::rom2049::dc::n64_name(name);
        if (mapped != name) {
            memset(&d[o], 0, size);
            memcpy(&d[o], mapped.data(), std::min(mapped.size(), size));
        }
    }

    // Placement (WORLDS): WHDR {u32 1, u32 offset, char name[16]}, WOBJ 0x68-byte records, GTLD s32 ids, GDAT 28-byte
    // quadtree nodes.
    bool convert_placement(const Bytes& dc, Bytes& out) {
        out = dc;
        std::map<std::string, Chunk> chunks;
        if (!swap_directory(out, chunks)) return false;
        const Chunk& whdr = chunks["WHDR"];
        const Chunk& wobj = chunks["WOBJ"];
        const Chunk& gtld = chunks["GTLD"];
        const Chunk& gdat = chunks["GDAT"];
        if (!whdr.found || !wobj.found) return false;
        if ((uint64_t)whdr.offset + 24 > dc.size() || (uint64_t)wobj.offset + (uint64_t)wobj.count * 0x68 > dc.size() ||
            (uint64_t)gtld.offset + (uint64_t)gtld.count * 4 > dc.size() || (uint64_t)gdat.offset + (uint64_t)gdat.count * 28 > dc.size()) {
            return false;
        }
        swap32s(out, whdr.offset, 2);
        for (uint32_t i = 0; i < wobj.count; i++) {
            size_t r = wobj.offset + i * 0x68;
            rename_field(out, r, 16);
            swap32s(out, r + 0x10, 13);  // matrix, position, flags
            swap16s(out, r + 0x44, 2);   // next, child
            swap32s(out, r + 0x48, 8);   // 0, dynamic id, bounding box
        }
        swap32s(out, gtld.offset, gtld.count);
        for (uint32_t i = 0; i < gdat.count; i++) {
            size_t r = gdat.offset + i * 28;
            uint16_t kind = le16(dc, r + 2);
            swap16s(out, r, 2);
            swap32s(out, r + 4, 4);
            if (kind == 0x10) {
                swap16s(out, r + 20, 2);
                swap32(out, r + 24);
            }
            else {
                swap16s(out, r + 20, 4);
            }
        }
        return true;
    }

    // Collision (ROAD): header u16 x6 + u32, then SEG (0x84), NODE (0x14), POLY (0x18), VERT (8), MOVER (0x20), VLIST
    // and LEAF bytes. The vertex and leaf lists are byte streams, stored as on the N64.
    bool convert_collision(const Bytes& dc, Bytes& out) {
        if (dc.size() < 0x10) return false;
        out = dc;
        uint32_t n_seg = le16(dc, 0), n_node = le16(dc, 2), n_poly = le16(dc, 4), n_vert = le16(dc, 6), n_mover = le16(dc, 8);
        uint32_t vlist_bytes = le16(dc, 10), leaf_bytes = le32(dc, 12);
        size_t seg = 0x10, node = seg + n_seg * 0x84, poly = node + n_node * 0x14, vert = poly + n_poly * 0x18,
               mover = vert + n_vert * 8, vlist = mover + n_mover * 0x20, leaf = vlist + vlist_bytes;
        if (leaf + leaf_bytes > dc.size()) return false;
        swap16s(out, 0, 6);
        swap32(out, 12);
        swap32s(out, seg, n_seg * 0x84 / 4);
        for (uint32_t i = 0; i < n_node; i++) {
            size_t r = node + i * 0x14;
            swap16(out, r);
            swap16s(out, r + 4, 4);
            swap16s(out, r + 0xC, 4);
        }
        for (uint32_t i = 0; i < n_poly; i++) {
            size_t r = poly + i * 0x18;
            swap16s(out, r, 12);
        }
        swap16s(out, vert, n_vert * 4);
        for (uint32_t i = 0; i < n_mover; i++) {
            size_t r = mover + i * 0x20;
            swap16s(out, r, 16);
        }
        return true;
    }
}

bool rush2::rom2049::dc::convert_file(const Files& files, int index, std::vector<uint8_t>& out,
                                      std::vector<SourceTexture>* shrunk) {
    Bytes dc;
    // AI paths: the same big-endian files on both.
    if (index >= 158 && index <= 176) {
        return files.get(track_base(index - 157) + "PATH.LZS", out);
    }
    if (index >= 177 && index <= 182) {
        return files.get(track_base(index - 176) + "PATHBACK.LZS", out);
    }
    if (index >= 120 && index <= 138) {
        return files.get(track_base(index - 119) + "WORLDS.LZS", dc) && convert_placement(dc, out);
    }
    if (index >= 139 && index <= 157) {
        return files.get(track_base(index - 138) + "ROAD.LZS", dc) && convert_collision(dc, out);
    }
    // Model containers.
    std::string model;
    if (index >= 101 && index <= 119) model = track_base(index - 100);
    else if (index >= 82 && index <= 87) model = "TARGETST" + std::to_string(index - 81);
    else if (index >= 88 && index <= 100) model = "CAR" + std::to_string(index - 87);
    else {
        switch (index) {
        case 56: model = "SELCAR"; break;
        case 60: model = "SELTRK"; break;
        case 61: case 62: model = "HUD"; break;
        case 63: model = "HUDBATTLE"; break;
        case 68: model = "TARGETSNOPAK"; break;
        case 76: model = "WEAPONS"; break;
        case 77: model = "WINGS"; break;
        case 78: case 79: model = "VEHICLES"; break;
        }
    }
    if (!model.empty()) return convert_model(files, model, index, out, shrunk);
    return false;
}
