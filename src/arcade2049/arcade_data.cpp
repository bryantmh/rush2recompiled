// Reader for San Francisco Rush 2049 arcade (SE) data as ProjectR unpacks it from the arcade hard drive: a port of
// tools/arcade2049/arcade.py, which documents every structure (and docs/arcade2049.md). All files are little endian
// and uncompressed.

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>

#include "texture_upscale.h"

#include "arcade_internal.h"

namespace a49 = rush2::arcade2049;

namespace {
    // Bounds-checked little-endian reads: ok turns false on a read past the end.
    struct Reader {
        const std::vector<uint8_t>& d;
        bool ok = true;
        bool has(size_t o, size_t n) {
            if (o > d.size() || n > d.size() - o) ok = false;
            return ok;
        }
        uint32_t u32(size_t o) {
            if (!has(o, 4)) return 0;
            return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
        }
        uint16_t u16(size_t o) { return has(o, 2) ? uint16_t(d[o] | d[o + 1] << 8) : 0; }
        int16_t s16(size_t o) { return int16_t(u16(o)); }
        int32_t s32(size_t o) { return int32_t(u32(o)); }
        float f32(size_t o) {
            uint32_t v = u32(o);
            float f;
            memcpy(&f, &v, 4);
            return f;
        }
        // NUL-terminated name in n bytes.
        std::string name(size_t o, size_t n) {
            if (!has(o, n)) return {};
            std::string s;
            for (size_t k = 0; k < n && d[o + k]; k++) s += char(d[o + k]);
            return s;
        }
    };

    int s9(int v) { return v & 256 ? v - 512 : v; }

    // Lines of a dobs script (CRLF), stripped, empty ones left out (Python's splitlines + strip).
    std::vector<std::string> script_lines(const std::string& text) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t k = 0; k <= text.size(); k++) {
            if (k == text.size() || text[k] == '\n' || text[k] == '\r') {
                // The arcade splits a script into words at ":=# \t\r\n" (ProjectR 0x1406EA215, its dobs parser's
                // separators), so "#followy 70 450" is followy, not a comment; a line here is still one command.
                std::string line = text.substr(start, k - start);
                for (char& c : line) if (c == ':' || c == '=' || c == '#') c = ' ';
                size_t a = line.find_first_not_of(" \t\f\v");
                size_t b = line.find_last_not_of(" \t\f\v");
                if (a != std::string::npos) out.push_back(line.substr(a, b - a + 1));
                start = k + 1;
            }
        }
        return out;
    }
}

bool a49::Texture::rgba(const std::vector<uint8_t>& texels, std::vector<uint8_t>& out) const {
    size_t n = size_t(w) * size_t(h);
    size_t bytes = fmt == fmt_yiq422 ? n : n * 2;
    if (w <= 0 || h <= 0 || offset > texels.size() || bytes > texels.size() - offset) return false;
    out.assign(n * 4, 255);
    const uint8_t* p = texels.data() + offset;
    for (size_t k = 0; k < n; k++) {
        if (fmt == fmt_rgb565) {
            int v = p[2 * k] | p[2 * k + 1] << 8;
            out[4 * k] = uint8_t(((v >> 11) & 31) * 255 / 31);
            out[4 * k + 1] = uint8_t(((v >> 5) & 63) * 255 / 63);
            out[4 * k + 2] = uint8_t((v & 31) * 255 / 31);
            continue;
        }
        int c, a = 255;
        if (fmt == fmt_yiq422) c = p[k];
        else if (fmt == fmt_ayiq8422) c = p[2 * k], a = p[2 * k + 1];
        else return false;
        for (int ch = 0; ch < 3; ch++) {
            int v = y[c >> 4] + i[(c >> 2) & 3][ch] + q[c & 3][ch];
            out[4 * k + ch] = uint8_t(std::clamp(v, 0, 255));
        }
        out[4 * k + 3] = uint8_t(a);
    }
    return true;
}

bool a49::Track::read(const std::string& name, std::vector<uint8_t>& out) const {
    std::ifstream f(dir / name, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

const std::vector<uint8_t>& a49::Track::texels() {
    if (!texels_read) {
        texels_read = true;
        read("textures.rom", texel_data);
    }
    return texel_data;
}

bool a49::Track::open(const std::filesystem::path& folder, std::string& error) {
    dir = folder;
    std::vector<uint8_t> d;
    if (!read("objects.rom", d)) {
        error = "no objects.rom in " + rush2::upscale::path_utf8(folder);
        return false;
    }
    Reader r{ d };
    // Header at 0x50: u32 object count, u32 0, u32 object name table, u32 texture name table; 0x64: u32 texture count.
    uint32_t n_obj = r.u32(0x50), obj_names = r.u32(0x58), tex_names = r.u32(0x5C), n_tex = r.u32(0x64);
    if (!r.ok || n_obj > 100000 || n_tex > 100000) {
        error = "objects.rom header unreadable";
        return false;
    }
    for (uint32_t k = 0; k < n_obj && r.ok; k++) {
        Object o;
        o.name = r.name(obj_names + 0x18 * k, 16);
        // The object header (0x8C bytes from 0x68): u32 0, f32 radius, u32 LOD count, 4 LOD slots of 0x20 bytes.
        size_t lo = 0x68 + 0x8C * size_t(k) + 0xC;
        uint32_t nv = r.u32(lo + 8), ov = r.u32(lo + 12), nt = r.u32(lo + 16), ot = r.u32(lo + 20);
        if (!r.has(ov, size_t(nv) * 28) || !r.has(ot, size_t(nt) * 8)) break;
        o.lod0.verts.resize(nv);
        for (uint32_t v = 0; v < nv; v++) {
            size_t vo = ov + 28 * size_t(v);
            Vertex& x = o.lod0.verts[v];
            x.pos = { r.f32(vo), r.f32(vo + 4), r.f32(vo + 8) };
            x.st = { r.f32(vo + 12), r.f32(vo + 16) };
            memcpy(x.rgba.data(), d.data() + vo + 22, 4);
        }
        o.lod0.tris.resize(nt);
        for (uint32_t t = 0; t < nt; t++) {
            for (int c = 0; c < 4; c++) o.lod0.tris[t][c] = r.u16(ot + 8 * size_t(t) + 2 * c);
        }
        objects.push_back(std::move(o));
    }
    size_t tex_desc = 0x68 + 0x8C * size_t(n_obj);
    for (uint32_t k = 0; k < n_tex && r.ok; k++) {
        size_t o = tex_desc + 0x50 * size_t(k);
        Texture t;
        t.name = r.name(tex_names + 0x14 * k, 16);
        t.fmt = r.has(o, 0x50) ? d[o + 2] : 0;
        t.w = r.u16(o + 4);
        t.h = r.u16(o + 6);
        t.offset = r.u32(o + 8);
        t.texture_mode = r.u32(o + 0x10);
        for (int j = 0; j < 16; j++) t.y[j] = r.has(o + 0x20 + j, 1) ? d[o + 0x20 + j] : 0;
        auto unpack = [](uint32_t v) { return std::array<int, 3>{ s9((v >> 18) & 511), s9((v >> 9) & 511), s9(v & 511) }; };
        for (int j = 0; j < 4; j++) {
            t.i[j] = unpack(r.u32(o + 0x30 + 4 * j));
            t.q[j] = unpack(r.u32(o + 0x40 + 4 * j));
        }
        textures.push_back(std::move(t));
    }
    if (!r.ok) {
        error = "objects.rom ends early";
        return false;
    }
    for (size_t k = 0; k < objects.size(); k++) by_name[objects[k].name] = k;
    return true;
}

const a49::Object* a49::Track::find(const std::string& name) const {
    auto it = by_name.find(name);
    return it == by_name.end() ? nullptr : &objects[it->second];
}

const a49::Object* a49::Track::object_named(const std::string& name) const {
    if (const Object* o = find(name)) return o;
    for (size_t cut : { size_t(15), size_t(8) }) {
        if (name.size() < cut) continue;
        std::vector<const Object*> hits;
        for (const Object& o : objects) {
            if (o.name.compare(0, cut, name, 0, cut) == 0 && o.name.size() >= cut && (cut == 15 || name.size() == 8)) {
                hits.push_back(&o);
            }
        }
        if (hits.size() == 1 || (!hits.empty() && cut == 15)) return hits[0];
    }
    return find(name + "_1");
}

bool a49::Track::worlds(std::vector<WorldEntry>& out, std::string& error) const {
    std::vector<uint8_t> w;
    if (!read("worlds.rom", w)) {
        error = "no worlds.rom";
        return false;
    }
    Reader r{ w };
    uint32_t first = r.u32(4);
    for (size_t o = first; o + 0x4C <= w.size(); o += 0x4C) {
        WorldEntry e;
        // char name[15], u8 1: a name that fills 8 characters runs on past the leftovers' place (AF_G3226 then '5'
        // is AF_G32265); when the long name isn't an object the first 8 are kept (arcade.py worlds).
        e.name = r.name(o, 8);
        std::string long_name = r.name(o, 15);
        if (long_name.size() > e.name.size() && by_name.count(long_name)) e.name = long_name;
        for (int k = 0; k < 9; k++) e.m[k] = r.f32(o + 16 + 4 * k);
        e.pos = { r.f32(o + 52), r.f32(o + 56), r.f32(o + 60) };
        out.push_back(std::move(e));
    }
    return true;
}

bool a49::Track::dobs(std::vector<DobsEntry>& out, std::string& error) const {
    std::vector<uint8_t> b;
    if (!read("dobs.rom", b)) {
        error = "no dobs.rom";
        return false;
    }
    Reader r{ b };
    size_t o = 1;
    while (o < b.size()) {
        size_t n = b[o++];
        if (n == 0 || o + n > b.size()) break;
        DobsEntry e;
        e.name = r.name(o, n);
        o += n;
        size_t sl = r.u16(o);
        o += 2;
        if (!r.has(o, sl)) break;
        std::string text(b.begin() + o, b.begin() + o + sl);
        text = text.substr(0, text.find('\0'));
        e.script = script_lines(text);
        o += sl;
        for (int k = 0; k < 9; k++) e.m[k] = r.f32(o + 4 * k);
        e.pos = { r.f32(o + 36), r.f32(o + 40), r.f32(o + 44) };
        o += 48;
        if (!r.ok) break;
        out.push_back(std::move(e));
    }
    return true;
}

// roaddat.rom is a run of blocks, each starting with its u32 size; the track's collision is the largest (arcade.py
// roaddat: the first on most tracks, after a 13-polygon block on TRACK6, Mission).
bool a49::Track::roaddat(std::vector<uint8_t>& out) const {
    std::vector<uint8_t> d;
    if (!read("roaddat.rom", d)) return false;
    size_t best = 0, best_at = 0;
    for (size_t o = 0; o + 28 <= d.size();) {
        size_t size = size_t(d[o]) | size_t(d[o + 1]) << 8 | size_t(d[o + 2]) << 16 | size_t(d[o + 3]) << 24;
        if (size < 28 || o + size > d.size()) break;
        if (size > best) best = size, best_at = o;
        o += size;
    }
    if (best == 0) out = std::move(d);
    else out.assign(d.begin() + best_at, d.begin() + best_at + best);
    return true;
}

bool a49::Track::collision(std::vector<CollisionPoly>& out, std::string& error) const {
    std::vector<uint8_t> d;
    if (!roaddat(d)) {
        error = "no roaddat.rom";
        return false;
    }
    Reader r{ d };
    uint32_t nn = r.u32(4), npl = r.u32(8), nv = r.u32(12), ns = r.u32(16), nleaf = r.u32(20);
    size_t po = 0x40 + size_t(nn) * 0x24;
    size_t vo = po + size_t(npl) * 0x1C;
    // The vertex lists follow the leaves (TRACK8's file ends in 2 bytes of padding, so not size - lists * 2).
    size_t vlo = vo + size_t(nv) * 12 + size_t(ns) * 0xA4 + size_t(nleaf) * 2;
    if (!r.has(vo, size_t(nv) * 12)) {
        error = "roaddat.rom ends early";
        return false;
    }
    auto vert = [&](size_t k) { return V3{ r.s32(vo + 12 * k) / 40.0, r.s32(vo + 12 * k + 4) / 40.0, r.s32(vo + 12 * k + 8) / 40.0 }; };
    for (uint32_t i = 0; i < npl; i++) {
        size_t p = po + 0x1C * size_t(i);
        uint32_t flags = r.u32(p);
        double m[9];
        for (int k = 0; k < 9; k++) m[k] = r.s16(p + 6 + 2 * k) / 16384.0;
        size_t vl = vlo + 2 * size_t(r.u32(p + 24));
        size_t cnt = r.u16(vl);
        if (!r.ok) break;
        if (cnt < 3) continue;
        std::vector<size_t> idx(cnt);
        for (size_t k = 0; k < cnt; k++) idx[k] = r.u16(vl + 2 + 2 * k);
        CollisionPoly poly;
        poly.flags = flags;
        V3 o = vert(idx[0]);
        poly.pts.push_back(to_world_path(o));
        for (size_t k = 1; k < cnt; k++) {
            V3 l = vert(idx[k]);
            V3 q;
            for (int a = 0; a < 3; a++) q[a] = o[a] + l[0] * m[3 * a] + l[1] * m[3 * a + 1] + l[2] * m[3 * a + 2];
            poly.pts.push_back(to_world_path(q));
        }
        // The face (the side cars drive on) is against column 2: road faces then point up.
        poly.normal = to_world_path({ -m[2], -m[5], -m[8] });
        if (flags & 0x3000) poly.seg = r.u16(vl + 2 + 2 * cnt);
        if (!r.ok) break;
        out.push_back(std::move(poly));
    }
    if (!r.ok) {
        error = "roaddat.rom ends early";
        return false;
    }
    return true;
}

bool a49::Track::segments(std::vector<std::array<double, 33>>& out, std::string& error) const {
    std::vector<uint8_t> d;
    if (!roaddat(d)) {
        error = "no roaddat.rom";
        return false;
    }
    Reader r{ d };
    uint32_t nn = r.u32(4), npl = r.u32(8), nv = r.u32(12), ns = r.u32(16);
    size_t at = 0x40 + size_t(nn) * 0x24 + size_t(npl) * 0x1C + size_t(nv) * 12;
    auto q = [](const V3& v) { return V3{ v[1], -v[2], v[0] }; };   // path frame -> world, for directions
    for (uint32_t i = 0; i < ns; i++) {
        double a[31];
        for (int k = 0; k < 31; k++) a[k] = r.f32(at + 0xA4 * size_t(i) + 4 * k);
        if (!r.ok) break;
        // world matrix = Q A^T Q^T: rows q(A's column 1), -q(column 2), q(column 0).
        V3 cols[3];
        for (int c = 0; c < 3; c++) cols[c] = q({ a[3 + c], a[6 + c], a[9 + c] });
        V3 mat[3] = { cols[1], cols[2] * -1.0, cols[0] };
        auto mix = [](const double* f, const double* g, double w) {
            V3 v{ f[0] * (1 - w) + g[0] * w, f[1] * (1 - w) + g[1] * w, f[2] * (1 - w) + g[2] * w };
            double n = norm(v);
            return v * (1.0 / (n > 0 ? n : 1.0));
        };
        std::array<double, 33> rec{};
        size_t k = 0;
        auto put = [&](const V3& v) { for (double x : v) rec[k++] = x; };
        put(q({ a[0], a[1], a[2] }));
        for (const V3& row : mat) put(row);
        put(q({ a[12], a[13], a[14] }));
        put(q(mix(a + 15, a + 18, 0.25)));
        put(q(mix(a + 15, a + 18, 0.75)));
        for (double v : { a[21] / 2, a[22], a[23], a[24], a[25] / 2, a[26] / 2, a[27] / 2, a[28] / 2,
                          a[29] * 0.75 + a[30] * 0.25, a[29] * 0.25 + a[30] * 0.75, 0.0, 0.0 }) {
            rec[k++] = v;
        }
        out.push_back(rec);
    }
    if (!r.ok) {
        error = "roaddat.rom's segments end early";
        return false;
    }
    return true;
}

bool a49::Track::path_tables(std::vector<PathTable>& out, std::string& error) const {
    std::vector<uint8_t> p;
    if (!read("dpath.rom", p)) {
        error = "no dpath.rom";
        return false;
    }
    Reader r{ p };
    // 8 tables of 2000 0x18-byte slots; slot 0 of each is its header (u32 lap point count, ...).
    for (size_t k = 0; k < p.size() / 48000; k++) {
        size_t base = k * 48000;
        PathTable t;
        t.lap_count = r.u32(base);
        for (size_t i = 1; i < 2000; i++) {
            size_t o = base + 0x18 * i;
            double x = r.f32(o), y = r.f32(o + 4), z = r.f32(o + 8), speed = r.f32(o + 12);
            uint32_t kind = r.u32(o + 16);
            if (!(x || y || z) && kind == 0) break;
            t.records.push_back({ to_world_path({ x, y, z }), speed, int(kind) });
        }
        out.push_back(std::move(t));
    }
    if (out.empty()) {
        error = "dpath.rom is too small";
        return false;
    }
    return true;
}

std::vector<uint8_t> a49::png(const std::vector<uint8_t>& rgba, int w, int h) {
    rush2::upscale::Image image;
    image.width = w;
    image.height = h;
    image.rgba = rgba;
    std::vector<uint8_t> out;
    rush2::upscale::write_png_bytes(image, out);
    return out;
}

std::filesystem::path a49::default_data_dir() {
    if (const char* env = std::getenv("ARCADE2049_DATA"); env && *env) return std::filesystem::u8path(env);
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    std::filesystem::path root = base ? std::filesystem::u8path(base) : std::filesystem::path();
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path root = xdg && *xdg ? std::filesystem::u8path(xdg)
                                             : std::filesystem::u8path(home ? home : "") / ".local" / "share";
#endif
    return root / "ProjectR" / "Data" / "sf2049se";
}

bool a49::is_data_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / "TRACKS" / "TRACK4" / "roaddat.rom", ec) &&
           std::filesystem::exists(dir / "CARS" / "CAR8", ec);
}

std::optional<std::filesystem::path> a49::find_data_dir(const std::filesystem::path& picked) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::is_directory(picked, ec) ? picked : picked.parent_path();
    for (int up = 0; up < 8 && !p.empty(); up++) {
        if (is_data_dir(p)) return p;
        if (is_data_dir(p / "sf2049se")) return p / "sf2049se";
        std::filesystem::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return std::nullopt;
}
