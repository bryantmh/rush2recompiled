// The PlayStation SF Rush disc reader (docs/sfrush_ps1.md#disc; tools/sfrush_ps1/ps1.py is the Python version): the
// data track is MODE2/2352 (2048 bytes of data at +24 in each 2352-byte sector) with an ISO 9660 file system.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "sfrush_ps1.h"
#include "texture_upscale.h"

namespace ps1 = rush2::sfrush_ps1;

namespace {
    constexpr uint32_t sector_size = 2352, data_offset = 24, data_size = 2048;

    uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

    std::string upper(std::string s) {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
        return s;
    }

    // The data track of a .cue: its first FILE line, next to the .cue.
    std::filesystem::path cue_track(const std::filesystem::path& cue) {
        std::ifstream f(cue);
        std::string line;
        while (std::getline(f, line)) {
            size_t k = line.find_first_not_of(" \t");
            if (k == std::string::npos || upper(line.substr(k, 4)) != "FILE") continue;
            size_t a = line.find('"'), b = line.rfind('"');
            if (a == std::string::npos || b <= a) continue;
            return cue.parent_path() / std::filesystem::u8path(line.substr(a + 1, b - a - 1));
        }
        return {};
    }
}

bool ps1::Disc::read_sectors(uint32_t lba, uint32_t size, std::vector<uint8_t>& out) const {
    std::ifstream f(bin, std::ios::binary);
    if (!f) return false;
    out.resize(size);
    for (uint32_t done = 0; done < size; lba++) {
        f.seekg(std::streamoff(lba) * sector_size + data_offset);
        uint32_t n = std::min(data_size, size - done);
        f.read(reinterpret_cast<char*>(out.data() + done), n);
        if (!f) return false;
        done += n;
    }
    return true;
}

bool ps1::Disc::walk(uint32_t lba, uint32_t size, const std::string& path, int depth) {
    if (depth > 8) return false;
    std::vector<uint8_t> dir;
    if (!read_sectors(lba, size, dir)) return false;
    for (size_t i = 0; i < dir.size();) {
        uint8_t n = dir[i];
        if (n == 0) {   // the rest of this sector is padding
            i = (i / data_size + 1) * data_size;
            continue;
        }
        if (i + 33 > dir.size() || i + n > dir.size()) break;
        const uint8_t* r = dir.data() + i;
        i += n;
        uint8_t name_len = r[32];
        if (33u + name_len > n) continue;
        std::string name(reinterpret_cast<const char*>(r + 33), name_len);
        if (name_len == 1 && (name[0] == 0 || name[0] == 1)) continue;   // . and ..
        if (size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
        std::string full = upper(path.empty() ? name : path + "/" + name);
        if (r[25] & 2) {
            if (!walk(u32(r + 2), u32(r + 10), full, depth + 1)) return false;
        } else {
            files[full] = { u32(r + 2), u32(r + 10) };
        }
    }
    return true;
}

bool ps1::Disc::open(const std::filesystem::path& path, std::string& error) {
    files.clear();
    std::string ext = upper(rush2::upscale::path_utf8(path.extension()));
    bin = ext == ".CUE" ? cue_track(path) : path;
    if (bin.empty()) {
        error = "the .cue names no data track";
        return false;
    }
    std::vector<uint8_t> pvd;
    if (!read_sectors(16, data_size, pvd) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        error = "not a PlayStation data track (MODE2/2352)";
        return false;
    }
    const uint8_t* root = pvd.data() + 156;
    if (!walk(u32(root + 2), u32(root + 10), "", 0) || files.empty()) {
        error = "the disc's file system couldn't be read";
        return false;
    }
    return true;
}

bool ps1::Disc::read(const std::string& name, std::vector<uint8_t>& out) const {
    auto it = files.find(upper(name));
    if (it == files.end()) return false;
    return read_sectors(it->second.first, it->second.second, out);
}

bool ps1::Disc::chunks(const std::string& base, std::vector<std::vector<uint8_t>>& out) const {
    std::vector<uint8_t> dat, siz;
    if (!read(base + ".DAT", dat) || !read(base + ".SIZ", siz)) return false;
    out.clear();
    size_t o = 0;
    for (size_t k = 0; k + 4 <= siz.size(); k += 4) {
        int32_t n = int32_t(u32(siz.data() + k));
        if (n < 0 || o + size_t(n) > dat.size()) return false;
        out.emplace_back(dat.begin() + o, dat.begin() + o + n);
        o += size_t(n);
    }
    return true;
}

bool ps1::is_disc(const std::filesystem::path& path) {
    Disc d;
    std::string error;
    return d.open(path, error) && d.has("SLUS_005.05") && d.has("EXE/RUSH.EXE") && d.has("T4ROAD/T4ROAD2.DAT") &&
           d.has("COLLDAT4/COLLMAP.DAT");
}

bool ps1::Exe::open(const Disc& disc) {
    std::vector<uint8_t> f;
    if (!disc.read("EXE/RUSH.EXE", f) || f.size() < 0x800 || std::memcmp(f.data(), "PS-X EXE", 8) != 0) return false;
    auto word = [&](size_t o) { return f[o] | f[o + 1] << 8 | f[o + 2] << 16 | uint32_t(f[o + 3]) << 24; };
    if (word(0x18) != base) return false;
    uint32_t size = word(0x1C);
    if (f.size() < 0x800 + size_t(size)) return false;
    data.assign(f.begin() + 0x800, f.begin() + 0x800 + size);
    jal_targets.clear();
    for (uint32_t a = 0; a + 4 <= data.size(); a += 4) {
        uint32_t ins = w(base + a);
        if ((ins >> 26) == 3) jal_targets.insert(((base + a) & 0xF0000000) | ((ins & 0x3FFFFFF) << 2));
    }
    return true;
}

uint32_t ps1::Exe::func_end(uint32_t a) const {
    auto it = jal_targets.upper_bound(a);
    return it == jal_targets.end() ? uint32_t(base + data.size()) : *it;
}

bool ps1::read_psv(const std::vector<uint8_t>& d, std::vector<std::array<double, 3>>& verts, std::vector<PsvFace>& faces) {
    auto u32 = [&](size_t o) { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24; };
    if (d.size() < 0x48) return false;
    uint32_t nv = u32(0x38), nf = u32(0x3C), vo = u32(0x40), fo = u32(0x44);
    if (vo + size_t(nv) * 12 > d.size()) return false;
    for (uint32_t i = 0; i < nv; i++) {
        size_t p = vo + 12 * size_t(i);
        verts.push_back({ int32_t(u32(p)) / 65536.0, int32_t(u32(p + 4)) / 65536.0, int32_t(u32(p + 8)) / 65536.0 });
    }
    size_t o = fo;
    for (uint32_t i = 0; i < nf; i++) {
        if (o + 8 > d.size()) return false;
        uint32_t n = u32(o + 4);
        if (n < 3 || n > 4 || o + 12 + 16 * size_t(n) > d.size()) return false;
        PsvFace f;
        f.flags = u32(o);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t v = u32(o + 8 + 4 * k);
            if (v >= nv) return false;
            f.idx.push_back(v);
            f.uv.push_back({ double(u32(o + 8 + 4 * n + 8 * k) >> 16), double(u32(o + 12 + 4 * n + 8 * k) >> 16) });
        }
        f.tex = u32(o + 8 + 12 * n);
        faces.push_back(std::move(f));
        o += 12 + 16 * size_t(n);
    }
    return true;
}
