// The N64 Rush 2049 code-segment tables the recomp reads, rebuilt from the Dreamcast executable (1ST_READ.BIN, loaded
// at 0x8C010000). See docs/rush2049_research/dreamcast.md ("Code tables").
//
// rush2049_dc_tables.inc (tools/rush2049/dc_tables.py) is a program of copies: each puts a disc table at its N64 address
// in the N64's layout, row order and index numbering. Schemas give an element's fields (the tool's docstring has the
// token list); strings and the data pointers point at go to a heap after the segment's N64 end, which the consumers
// read like the rest of the segment.

#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include "rush2049_dc_internal.h"

namespace {
    using Bytes = std::vector<uint8_t>;
    using rush2::rom2049::dc::exe_base;

    struct SegmentSize {
        int segment;
        uint32_t size;
    };
    const SegmentSize segment_sizes[] = {
#define SEGMENT(segment, size) { segment, size },
#include "rush2049_dc_tables.inc"
    };

    struct Schema {
        int id;
        const char* layout;
        int end;    // 0 none, 1 a first s16 of 0 ends a list, 2 a first word of 0
    };
    const Schema schemas[] = {
#define SCHEMA(id, layout, end) { id, layout, end },
#include "rush2049_dc_tables.inc"
    };

    struct Op {
        int kind;   // 0 copy, 1 string, 2 word
        int segment;
        uint32_t n64;
        uint32_t dc;
        int schema, count;
        const char* text;
    };
    const Op ops[] = {
#define COPY(segment, n64, dc, schema, count) { 0, segment, n64, dc, schema, count, nullptr },
#define STR(segment, n64, text) { 1, segment, n64, 0, 0, 0, text },
#define WORD(segment, n64, value) { 2, segment, n64, value, 0, 0, nullptr },
#include "rush2049_dc_tables.inc"
    };

    struct Value {
        int map;
        int32_t dc, n64;
    };
    const Value values[] = {
#define VALUE(map, dc, n64) { map, dc, n64 },
#include "rush2049_dc_tables.inc"
    };

    struct Func {
        uint32_t dc, n64;
    };
    const Func funcs[] = {
#define FUNC(dc, n64) { dc, n64 },
#include "rush2049_dc_tables.inc"
    };

    struct Bad {};

    class Builder {
    public:
        Builder(const Bytes& exe, rush2::rom2049::Segment s, Bytes& out) : exe_(exe), seg_(out), vram_(rush2::rom2049::segment_vram(s)) {}

        void run(int segment) {
            for (const Op& op : ops) {
                if (op.segment != segment) continue;
                if (op.kind == 0) {
                    const Schema& sc = schema(op.schema);
                    auto [dsz, nsz] = sizes(sc.layout);
                    for (int i = 0; i < op.count; i++) element(op.n64 + i * nsz, op.dc + i * dsz, sc);
                }
                else if (op.kind == 1) {
                    put32(op.n64, string(op.text));
                }
                else {
                    put32(op.n64, op.dc);
                }
            }
        }

    private:
        const Bytes& exe_;
        Bytes& seg_;
        uint32_t vram_;
        std::map<std::string, uint32_t> strings_;

        static const Schema& schema(int id) {
            for (const Schema& s : schemas) {
                if (s.id == id) return s;
            }
            throw Bad{};
        }

        // Disc and N64 sizes of an element.
        static std::pair<uint32_t, uint32_t> sizes(const char* layout) {
            uint32_t dc = 0, n64 = 0;
            for (const char* p = layout; *p; p++) {
                if (p != layout && p[-1] != ' ') continue;
                switch (*p) {
                case 'w': case 'r': case 's': case 'f': case 'P': case 'Q': case 'F': case 'V': dc += 4; n64 += 4; break;
                case 'h': case 'H': dc += 2; n64 += 2; break;
                case 'b': dc += 1; n64 += 1; break;
                case 'X': dc += 4; break;
                case 'z': n64 += 4; break;
                }
            }
            return { dc, n64 };
        }

        uint32_t dw(uint32_t a) const {
            size_t o = a - exe_base;
            if (a < exe_base || o + 4 > exe_.size()) throw Bad{};
            return exe_[o] | (exe_[o + 1] << 8) | (exe_[o + 2] << 16) | ((uint32_t)exe_[o + 3] << 24);
        }
        uint16_t dh(uint32_t a) const {
            size_t o = a - exe_base;
            if (a < exe_base || o + 2 > exe_.size()) throw Bad{};
            return uint16_t(exe_[o] | (exe_[o + 1] << 8));
        }
        uint8_t db(uint32_t a) const {
            size_t o = a - exe_base;
            if (a < exe_base || o >= exe_.size()) throw Bad{};
            return exe_[o];
        }
        std::string dstr(uint32_t a) const {
            size_t o = a - exe_base;
            if (a < exe_base || o >= exe_.size()) throw Bad{};
            size_t e = o;
            while (e < exe_.size() && exe_[e]) e++;
            return std::string((const char*)&exe_[o], e - o);
        }

        uint8_t* at(uint32_t n64, size_t n) {
            size_t o = n64 - vram_;
            if (n64 < vram_ || o + n > seg_.size()) throw Bad{};
            return &seg_[o];
        }
        void put32(uint32_t n64, uint32_t v) {
            uint8_t* p = at(n64, 4);
            p[0] = uint8_t(v >> 24);
            p[1] = uint8_t(v >> 16);
            p[2] = uint8_t(v >> 8);
            p[3] = uint8_t(v);
        }
        void put16(uint32_t n64, uint16_t v) {
            uint8_t* p = at(n64, 2);
            p[0] = uint8_t(v >> 8);
            p[1] = uint8_t(v);
        }

        uint32_t heap(size_t n, size_t align) {
            while (seg_.size() % align) seg_.push_back(0);
            uint32_t a = vram_ + (uint32_t)seg_.size();
            seg_.resize(seg_.size() + n, 0);
            return a;
        }

        // Strings are object names: renamed to the N64's.
        uint32_t string(const std::string& text) {
            std::string s = rush2::rom2049::dc::n64_name(text);
            auto it = strings_.find(s);
            if (it != strings_.end()) return it->second;
            uint32_t a = heap(s.size() + 1, 1);
            memcpy(at(a, s.size()), s.data(), s.size());
            strings_[s] = a;
            return a;
        }

        uint32_t array(uint32_t dc, const Schema& sc, int count) {
            auto [dsz, nsz] = sizes(sc.layout);
            if (count < 0) {
                count = 0;
                for (;;) {
                    uint32_t a = dc + count * dsz;
                    count++;
                    if ((sc.end == 1 && dh(a) == 0) || (sc.end == 2 && dw(a) == 0)) break;
                    if (count > 4096) throw Bad{};
                }
            }
            uint32_t to = heap((size_t)nsz * count, 4);
            for (int i = 0; i < count; i++) element(to + i * nsz, dc + i * dsz, sc);
            return to;
        }

        void element(uint32_t n64, uint32_t dc, const Schema& sc) {
            uint32_t start = dc;
            const char* p = sc.layout;
            while (*p) {
                char c = *p++;
                int a = 0, b = 0;
                bool has_b = false;
                while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
                if (*p == ',') {
                    p++;
                    has_b = true;
                    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
                }
                while (*p == ' ') p++;
                switch (c) {
                case 'w':
                    put32(n64, dw(dc)); n64 += 4; dc += 4; break;
                case 'r': {
                    uint32_t v = dw(dc);
                    put32(n64, (v >> 16) | (v << 16)); n64 += 4; dc += 4; break;
                }
                case 'h':
                    put16(n64, dh(dc)); n64 += 2; dc += 2; break;
                case 'H': {
                    int16_t v = (int16_t)dh(dc);
                    if (v >= 0) {
                        int16_t mapped = -1;
                        for (const Value& m : values) {
                            if (m.map == a && m.dc == v) mapped = (int16_t)m.n64;
                        }
                        v = mapped;
                    }
                    put16(n64, (uint16_t)v); n64 += 2; dc += 2; break;
                }
                case 'V': {
                    int32_t v = (int32_t)dw(dc);
                    if (v >= 0) {
                        int32_t mapped = -1;
                        for (const Value& m : values) {
                            if (m.map == a && m.dc == v) mapped = m.n64;
                        }
                        v = mapped;
                    }
                    put32(n64, (uint32_t)v); n64 += 4; dc += 4; break;
                }
                case 'b':
                    *at(n64, 1) = db(dc); n64 += 1; dc += 1; break;
                case 's': {
                    uint32_t ptr = dw(dc);
                    put32(n64, ptr ? string(dstr(ptr)) : 0); n64 += 4; dc += 4; break;
                }
                case 'f': {
                    uint32_t ptr = dw(dc), mapped = 0;
                    for (const Func& f : funcs) {
                        if (f.dc == ptr) mapped = f.n64;
                    }
                    put32(n64, mapped); n64 += 4; dc += 4; break;
                }
                case 'P': case 'Q': case 'F': {
                    uint32_t ptr = dw(dc), target = 0;
                    if (ptr) {
                        int count = c == 'P' ? (has_b ? b : 1) : c == 'F' ? std::max<int>(0, (int16_t)dh(start)) : -1;
                        target = array(ptr, schema(a), count);
                    }
                    put32(n64, target); n64 += 4; dc += 4; break;
                }
                case 'X':
                    dc += 4; break;
                case 'z':
                    put32(n64, 0); n64 += 4; break;
                default:
                    throw Bad{};
                }
            }
        }
    };

    // Probes of the disc tables' places: what this map was made for (the USA disc). Self-boot copies may patch the
    // executable elsewhere, so the whole file isn't hashed.
    bool probe_string(const Bytes& exe, uint32_t pointer_at, const char* text) {
        size_t o = pointer_at - exe_base;
        if (pointer_at < exe_base || o + 4 > exe.size()) return false;
        uint32_t p = exe[o] | (exe[o + 1] << 8) | (exe[o + 2] << 16) | ((uint32_t)exe[o + 3] << 24);
        size_t s = p - exe_base, n = strlen(text);
        return p >= exe_base && s + n + 1 <= exe.size() && memcmp(&exe[s], text, n + 1) == 0;
    }
}

bool rush2::rom2049::dc::known_executable(const std::vector<uint8_t>& exe) {
    static const uint8_t pvs_counts[] = { 117, 117, 117, 121, 103, 122 };
    size_t pvs = 0x8C0C7860 - exe_base;
    return probe_string(exe, 0x8C0BFD80, "GOLDCOIN") &&      // type table
           probe_string(exe, 0x8C0AE1E8, "CACTUSG2") &&      // model handle names
           probe_string(exe, 0x8C0C31BC, "BRI_WATER2") &&  // first scroll of track 1
           pvs + sizeof(pvs_counts) <= exe.size() && memcmp(&exe[pvs], pvs_counts, sizeof(pvs_counts)) == 0;
}

bool rush2::rom2049::dc::build_segment(const std::vector<uint8_t>& exe, Segment s, std::vector<uint8_t>& out) {
    int id = s == Segment::Boot ? 0 : s == Segment::Main ? 1 : 2;
    out.clear();
    for (const SegmentSize& z : segment_sizes) {
        if (z.segment == id) out.assign(z.size, 0);
    }
    if (out.empty()) return false;
    try {
        Builder b(exe, s, out);
        b.run(id);
        return true;
    }
    catch (const Bad&) {
        out.clear();
        return false;
    }
}
