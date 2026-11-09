// The PlayStation SF Rush disc reader (docs/sfrush_ps1.md#disc; tools/sfrush_ps1/ps1.py is the Python version): the
// data track is MODE2/2352 (2048 bytes of data at +24 in each 2352-byte sector) with an ISO 9660 file system.
// Images: a .cue and its .bin files, a CloneCD .ccd/.img (the .img is the whole disc in raw sectors, the .ccd lists
// its tracks), a .chd (chdman's compressed .cue; include/chd_image.h), or the data track alone. A data track file's
// sector layout is found from where the volume descriptor ("\1CD001" at sector 16) sits: raw 2352 (+24, or +16 for a
// MODE1 rip), 2048 (an .iso, which keeps no music) or 2336 (+8).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "chd_image.h"
#include "sfrush_ps1.h"
#include "texture_upscale.h"

namespace ps1 = rush2::sfrush_ps1;

namespace {
    constexpr uint32_t raw_sector = 2352, data_size = 2048;

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

    // The file next to `path` with its name and extension `ext` (upper case; any case matches), or empty.
    std::filesystem::path sibling(const std::filesystem::path& path, const char* ext) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(path.parent_path(), ec)) {
            if (e.path().stem() == path.stem() && upper(e.path().extension().string()) == ext) return e.path();
        }
        return {};
    }

    // A .ccd's audio tracks: "[TRACK n]" sections with MODE=0 for audio and "INDEX 1=<sector>" (INDEX 0 for a
    // pregap), counting 2352-byte sectors from the start of the .img. A track runs to the next one's INDEX 0 (or 1).
    std::vector<ps1::AudioTrack> ccd_audio(const std::filesystem::path& ccd) {
        struct T { int number = 0; bool audio = false; int64_t index0 = -1, index1 = -1; };
        std::vector<T> tracks;
        std::ifstream f(ccd);
        std::string line;
        bool in_track = false;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string u = upper(line);
            if (u.rfind("[", 0) == 0) {
                in_track = u.rfind("[TRACK ", 0) == 0;
                if (in_track) tracks.push_back({ std::atoi(u.c_str() + 7) });
                continue;
            }
            if (!in_track) continue;
            if (u.rfind("MODE=", 0) == 0) tracks.back().audio = std::atoi(u.c_str() + 5) == 0;
            else if (u.rfind("INDEX 0=", 0) == 0) tracks.back().index0 = std::atoll(u.c_str() + 8);
            else if (u.rfind("INDEX 1=", 0) == 0) tracks.back().index1 = std::atoll(u.c_str() + 8);
        }
        std::filesystem::path img = sibling(ccd, ".IMG");
        std::error_code ec;
        uint64_t size = img.empty() ? 0 : std::filesystem::file_size(img, ec);
        std::vector<ps1::AudioTrack> out;
        if (ec || size == 0) return out;
        for (size_t i = 0; i < tracks.size(); i++) {
            const T& t = tracks[i];
            if (!t.audio || t.index1 < 0) continue;
            uint64_t start = uint64_t(t.index1) * raw_sector, end = size;
            if (i + 1 < tracks.size()) {
                int64_t next = tracks[i + 1].index0 >= 0 ? tracks[i + 1].index0 : tracks[i + 1].index1;
                if (next >= 0) end = std::min<uint64_t>(end, uint64_t(next) * raw_sector);
            }
            if (end > start) out.push_back({ img, start, end - start, t.number });
        }
        return out;
    }

    // A .chd's audio tracks: `offset` is the track's first CHD frame.
    std::vector<ps1::AudioTrack> chd_audio(const std::filesystem::path& path) {
        rush2::chd::Image image;
        std::vector<ps1::AudioTrack> out;
        if (!image.open(path)) return out;
        for (const rush2::chd::Track& t : image.tracks()) {
            if (t.audio && t.frames > 0) {
                out.push_back({ path, t.chd_frame, uint64_t(t.frames) * raw_sector, t.number, true });
            }
        }
        return out;
    }
}

bool ps1::Disc::read_sectors(uint32_t lba, uint32_t size, std::vector<uint8_t>& out) const {
    out.resize(size);
    if (chd) {
        uint8_t frame[rush2::chd::frame_size];
        for (uint32_t done = 0; done < size; lba++) {
            if (!chd->read_frame(chd_frame + lba, frame)) return false;
            uint32_t n = std::min(data_size, size - done);
            std::memcpy(out.data() + done, frame + data_offset, n);
            done += n;
        }
        return true;
    }
    std::ifstream f(bin, std::ios::binary);
    if (!f) return false;
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
    chd.reset();
    chd_frame = 0;
    std::string ext = upper(rush2::upscale::path_utf8(path.extension()));
    auto is_volume = [](const std::vector<uint8_t>& pvd) {
        return pvd.size() >= 6 && pvd[0] == 1 && std::memcmp(pvd.data() + 1, "CD001", 5) == 0;
    };
    std::vector<uint8_t> pvd;
    if (ext == ".CHD") {
        auto image = std::make_shared<rush2::chd::Image>();
        if (!image->open(path)) {
            error = "the .chd couldn't be read";
            return false;
        }
        auto data = std::find_if(image->tracks().begin(), image->tracks().end(), [](const auto& t) { return !t.audio; });
        if (data == image->tracks().end()) {
            error = "the .chd has no data track";
            return false;
        }
        bin = path;
        sector_size = raw_sector;
        data_offset = data->header;
        chd_frame = data->chd_frame;
        chd = image;
        if (!read_sectors(16, data_size, pvd) || !is_volume(pvd)) {
            error = "the .chd's data track holds no CD file system";
            return false;
        }
    }
    else {
        bin = ext == ".CUE" ? cue_track(path) : ext == ".CCD" ? sibling(path, ".IMG") : path;
        if (bin.empty()) {
            error = ext == ".CCD" ? "no .img next to the .ccd" : "the .cue names no data track";
            return false;
        }
        static constexpr std::pair<uint32_t, uint32_t> layouts[] = { { 2352, 24 }, { 2048, 0 }, { 2336, 8 }, { 2352, 16 } };
        bool found = false;
        for (auto [size, offset] : layouts) {
            sector_size = size;
            data_offset = offset;
            if (read_sectors(16, data_size, pvd) && is_volume(pvd)) {
                found = true;
                break;
            }
        }
        if (!found) {
            error = "not a CD data track";
            return false;
        }
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

// The .cue's CD audio tracks (the disc's music, src/music_streams.cpp): FILE names a .bin (next to the .cue), TRACK nn
// AUDIO starts a track and INDEX 01 mm:ss:ff (75 sectors a second, 2352 bytes each) is where its audio begins in that
// file. A track runs to the next track's INDEX 00 (its pregap) or INDEX 01 in the same file, else to the file's end.
// Given the data track's .bin, the .cue next to it that names it is read.
std::vector<ps1::AudioTrack> ps1::audio_tracks(const std::filesystem::path& path) {
    std::string ext = upper(path.extension().string());
    if (ext == ".CHD") return chd_audio(path);
    if (ext == ".CCD") return ccd_audio(path);
    if (ext == ".IMG") {
        std::filesystem::path ccd = sibling(path, ".CCD");
        if (!ccd.empty()) return ccd_audio(ccd);
    }
    std::filesystem::path cue = path;
    std::error_code ec;
    if (ext != ".CUE") {
        cue.clear();
        for (const auto& e : std::filesystem::directory_iterator(path.parent_path(), ec)) {
            if (upper(e.path().extension().string()) == ".CUE" && cue_track(e.path()) == path) {
                cue = e.path();
                break;
            }
        }
        if (cue.empty()) return {};
    }
    struct Mark {
        int track = 0;
        bool audio = false;
        std::filesystem::path file;
        uint64_t start = 0;     // INDEX 01
        uint64_t end_mark = 0;  // its INDEX 00 (or INDEX 01): where the track before it ends
    };
    std::vector<Mark> marks;
    std::ifstream f(cue);
    std::string line;
    std::filesystem::path file;
    while (std::getline(f, line)) {
        size_t k = line.find_first_not_of(" \t");
        if (k == std::string::npos) continue;
        std::string word = upper(line.substr(k, line.find_first_of(" \t", k) - k));
        if (word == "FILE") {
            size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) file = cue.parent_path() / std::filesystem::u8path(line.substr(a + 1, b - a - 1));
        }
        else if (word == "TRACK") {
            Mark m;
            m.track = std::atoi(line.c_str() + k + 5);
            m.audio = upper(line).find("AUDIO") != std::string::npos;
            m.file = file;
            m.start = m.end_mark = UINT64_MAX;
            marks.push_back(m);
        }
        else if (word == "INDEX" && !marks.empty()) {
            int index = 0, mm = 0, ss = 0, ff = 0;
            if (sscanf(line.c_str() + k + 5, "%d %d:%d:%d", &index, &mm, &ss, &ff) != 4) continue;
            uint64_t at = (uint64_t(mm) * 60 * 75 + uint64_t(ss) * 75 + uint64_t(ff)) * raw_sector;
            Mark& m = marks.back();
            if (index == 0) m.end_mark = at;
            if (index == 1) {
                m.start = at;
                if (m.end_mark == UINT64_MAX) m.end_mark = at;
            }
        }
    }
    std::vector<AudioTrack> out;
    for (size_t i = 0; i < marks.size(); i++) {
        const Mark& m = marks[i];
        if (!m.audio || m.start == UINT64_MAX) continue;
        uint64_t size = std::filesystem::file_size(m.file, ec);
        if (ec || size <= m.start) continue;
        uint64_t end = size;
        if (i + 1 < marks.size() && marks[i + 1].file == m.file && marks[i + 1].end_mark != UINT64_MAX) {
            end = std::min(end, marks[i + 1].end_mark);
        }
        if (end > m.start) out.push_back({ m.file, m.start, end - m.start, m.track });
    }
    return out;
}

// A track's samples: the bytes from the .bin/.img, or the .chd's frames (read_frame swaps them back to little-endian).
bool ps1::read_audio(const AudioTrack& track, std::vector<uint8_t>& out) {
    if (track.chd) {
        rush2::chd::Image image;
        if (!image.open(track.file)) return false;
        uint32_t frames = uint32_t(track.length / raw_sector);
        out.resize(size_t(frames) * raw_sector);
        for (uint32_t i = 0; i < frames; i++) {
            if (!image.read_frame(uint32_t(track.offset) + i, out.data() + size_t(i) * raw_sector)) return false;
        }
        return true;
    }
    std::ifstream f(track.file, std::ios::binary);
    if (!f) return false;
    out.resize((size_t)track.length);
    f.seekg((std::streamoff)track.offset);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)track.length);
    return (bool)f;
}
