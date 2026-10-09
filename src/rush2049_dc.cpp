// The Dreamcast Rush 2049 (USA) disc as a Rush 2049 source. See include/rush2049_dc.h and
// docs/rush2049_research/dreamcast.md.
//
// Disc images: a .cdi (DiscJuggler) or .iso is one file of sectors; a .gdi lists the GD-ROM's tracks, each its own
// file. Sectors are 2048 bytes of data, 2336 (8-byte mode 2 subheader first) or 2352 (raw, 16-byte header). The game
// is an ISO 9660 volume whose primary volume descriptor ("\1CD001" at logical sector 16) is found by scanning; the
// first volume holding 1ST_READ.BIN, TRACK1.LZS and SELTRK.LZS with every file inside the image is the game's (a
// self-booting .cdi keeps a second copy of the directory whose files lie past the end of the image).
//
// The pack (rush2049_dc.pak in the app folder): "R49DCPAK", u32 version, u32 entry count, u32 length + the image's
// path (UTF-8), then per entry { u8 name length, name, u64 offset, u64 size, u32 kind }, then the data. Kind 0 is a
// file as the disc stores it (.LZS files stay compressed). Kind 1 is a song (.STR): the disc's 2048-byte header, then
// the music as IMA ADPCM (adpcm_encode), block by block.
//
// .STR songs on the disc: header u32 1, u32 sample rate (22050), u32 bits (16), u32 block size (0x4000), u32 block
// count, u32 data bytes, u32 channels (2), then loop fields; the data from 0x800 is PCM16 in 0x4000-byte blocks per
// channel, left block then right block.

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "rush2049_dc.h"
#include "rush2049_dc_internal.h"
#include "track_cache.h"
#include "wings_internal.h"

namespace fs = std::filesystem;
using rush2::rom2049::dc::ImportResult;

namespace {
    constexpr char pack_magic[8] = { 'R', '4', '9', 'D', 'C', 'P', 'A', 'K' };
    constexpr uint32_t pack_version = 1;
    enum class Kind : uint32_t { Raw = 0, Music = 1 };

    uint32_t le32(const uint8_t* p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    std::string upper(std::string s) {
        for (char& c : s) {
            c = (char)toupper((unsigned char)c);
        }
        return s;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Disc images

    struct Track {
        std::shared_ptr<std::ifstream> file;
        uint32_t start_lba = 0;   // First logical sector in the file.
        uint64_t file_offset = 0; // Where that sector starts.
        uint32_t sector_size = 2048;
        uint32_t header = 0;      // Bytes before a sector's 2048 bytes of data.
        uint64_t sectors = 0;
    };

    class Disc {
    public:
        std::vector<Track> tracks; // By start_lba.

        // Appends count sectors' data from lba. False past the end of the image.
        bool read(uint32_t lba, uint32_t count, std::vector<uint8_t>& out) const {
            size_t at = out.size();
            out.resize(at + size_t(count) * 2048);
            for (uint32_t i = 0; i < count; i++) {
                const Track* t = nullptr;
                for (const Track& c : tracks) {
                    if (lba + i >= c.start_lba && lba + i < c.start_lba + c.sectors) {
                        t = &c;
                    }
                }
                if (t == nullptr) {
                    return false;
                }
                // Runs of sectors in the same track with 2048-byte sectors are read in one go.
                uint32_t run = 1;
                if (t->sector_size == 2048) {
                    run = (uint32_t)std::min<uint64_t>(count - i, t->start_lba + t->sectors - (lba + i));
                    t->file->seekg((std::streamoff)(t->file_offset + uint64_t(lba + i - t->start_lba) * 2048));
                    t->file->read((char*)&out[at + size_t(i) * 2048], std::streamsize(run) * 2048);
                }
                else {
                    t->file->seekg((std::streamoff)(t->file_offset + uint64_t(lba + i - t->start_lba) * t->sector_size + t->header));
                    t->file->read((char*)&out[at + size_t(i) * 2048], 2048);
                }
                if (!*t->file) {
                    t->file->clear();
                    return false;
                }
                i += run - 1;
            }
            return true;
        }
    };

    struct Entry {
        uint32_t lba;
        uint32_t size;
    };

    // The files of the ISO 9660 volume whose descriptor is at logical sector pvd_lba (directories flattened; names
    // upper case, without ";1").
    bool read_volume(const Disc& disc, uint32_t pvd_lba, std::map<std::string, Entry>& files) {
        std::vector<uint8_t> pvd;
        if (!disc.read(pvd_lba, 1, pvd) || pvd[0] != 1 || memcmp(&pvd[1], "CD001", 5) != 0) {
            return false;
        }
        std::vector<std::pair<std::string, Entry>> dirs = { { "", { le32(&pvd[156 + 2]), le32(&pvd[156 + 10]) } } };
        std::set<uint32_t> seen;
        while (!dirs.empty()) {
            auto [path, dir] = dirs.back();
            dirs.pop_back();
            if (!seen.insert(dir.lba).second || dir.size > 0x100000) {
                continue;
            }
            std::vector<uint8_t> d;
            if (!disc.read(dir.lba, (dir.size + 2047) / 2048, d)) {
                return false;
            }
            for (size_t i = 0; i < d.size();) {
                uint8_t n = d[i];
                if (n == 0) {
                    i = (i / 2048 + 1) * 2048;
                    continue;
                }
                if (i + n > d.size() || n < 34) {
                    break;
                }
                const uint8_t* r = &d[i];
                uint8_t name_len = r[32];
                std::string name((const char*)r + 33, std::min<size_t>(name_len, n - 33));
                i += n;
                if (name_len == 1 && (name[0] == 0 || name[0] == 1)) {
                    continue;
                }
                if (size_t semi = name.find(';'); semi != std::string::npos) {
                    name.resize(semi);
                }
                Entry e{ le32(r + 2), le32(r + 10) };
                std::string full = path.empty() ? upper(name) : path + "/" + upper(name);
                if (r[25] & 2) {
                    dirs.push_back({ full, e });
                }
                else {
                    files[full] = e;
                }
            }
        }
        return true;
    }

    bool is_game_volume(const Disc& disc, const std::map<std::string, Entry>& files) {
        for (const char* required : { "1ST_READ.BIN", "TRACK1.LZS", "SELTRK.LZS" }) {
            auto it = files.find(required);
            if (it == files.end()) {
                return false;
            }
            // The last sector of the file must be in the image.
            std::vector<uint8_t> probe;
            uint32_t last = it->second.lba + (std::max<uint32_t>(it->second.size, 1) - 1) / 2048;
            if (!disc.read(last, 1, probe)) {
                return false;
            }
        }
        return true;
    }

    // Finds the game's volume in a single-file image (.cdi, .iso): scans for volume descriptors and tries each sector
    // layout one could sit in.
    bool open_single_file(const fs::path& path, Disc& disc, std::map<std::string, Entry>& files) {
        auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!*file) {
            return false;
        }
        file->seekg(0, std::ios::end);
        uint64_t size = (uint64_t)file->tellg();
        static const uint8_t signature[7] = { 1, 'C', 'D', '0', '0', '1', 1 };
        constexpr size_t chunk = 8 << 20;
        std::vector<uint8_t> buf(chunk + sizeof(signature));
        struct Layout { uint32_t size, header; };
        constexpr Layout layouts[] = { { 2048, 0 }, { 2336, 8 }, { 2352, 16 }, { 2352, 24 } };
        for (uint64_t at = 0; at < size; at += chunk) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), size - at);
            file->clear();
            file->seekg((std::streamoff)at);
            file->read((char*)buf.data(), (std::streamsize)n);
            for (size_t i = 0; i + sizeof(signature) <= n && i < chunk; i++) {
                if (buf[i] != 1 || memcmp(&buf[i], signature, sizeof(signature)) != 0) {
                    continue;
                }
                uint64_t pos = at + i;
                for (const Layout& l : layouts) {
                    if (pos < l.header || (pos - l.header) % l.size != 0 || (pos - l.header) / l.size < 16) {
                        continue;
                    }
                    uint64_t base = (pos - l.header) / l.size - 16;
                    Disc d;
                    d.tracks.push_back({ file, 0, base * l.size, l.size, l.header, (size - base * l.size) / l.size });
                    std::map<std::string, Entry> f;
                    if (read_volume(d, 16, f) && is_game_volume(d, f)) {
                        disc = std::move(d);
                        files = std::move(f);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // A .gdi: "count", then per track "number start_lba type sector_size file offset" (type 4 = data). The high-density
    // area's volume descriptor is at the data track starting at LBA 45000, + 16; directory sectors are absolute.
    bool open_gdi(const fs::path& path, Disc& disc, std::map<std::string, Entry>& files) {
        std::ifstream list(path);
        if (!list) {
            return false;
        }
        int count = 0;
        list >> count;
        std::string line;
        std::getline(list, line);
        Disc d;
        while (std::getline(list, line)) {
            // The file name may be quoted and hold spaces.
            std::vector<std::string> fields;
            for (size_t i = 0; i < line.size();) {
                while (i < line.size() && isspace((unsigned char)line[i])) i++;
                if (i >= line.size()) break;
                std::string f;
                if (line[i] == '"') {
                    size_t e = line.find('"', i + 1);
                    f = line.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
                    i = e == std::string::npos ? line.size() : e + 1;
                }
                else {
                    size_t e = i;
                    while (e < line.size() && !isspace((unsigned char)line[e])) e++;
                    f = line.substr(i, e - i);
                    i = e;
                }
                fields.push_back(f);
            }
            if (fields.size() < 5 || fields[2] != "4") {
                continue;
            }
            uint32_t sector_size = (uint32_t)std::stoul(fields[3]);
            if (sector_size != 2048 && sector_size != 2352 && sector_size != 2336) {
                continue;
            }
            auto file = std::make_shared<std::ifstream>(path.parent_path() / fs::u8path(fields[4]), std::ios::binary);
            if (!*file) {
                return false;
            }
            file->seekg(0, std::ios::end);
            uint64_t size = (uint64_t)file->tellg();
            uint32_t header = sector_size == 2352 ? 16 : sector_size == 2336 ? 8 : 0;
            d.tracks.push_back({ file, (uint32_t)std::stoul(fields[1]), 0, sector_size, header, size / sector_size });
        }
        std::map<std::string, Entry> f;
        if (read_volume(d, 45000 + 16, f) && is_game_volume(d, f)) {
            disc = std::move(d);
            files = std::move(f);
            return true;
        }
        return false;
    }

    bool read_entry(const Disc& disc, const Entry& e, std::vector<uint8_t>& out) {
        out.clear();
        if (!disc.read(e.lba, (e.size + 2047) / 2048, out)) {
            return false;
        }
        out.resize(e.size);
        return true;
    }

    // Disc files the pack keeps: everything the game reads but movies, the network browser and boot files.
    bool wanted(const std::string& name) {
        if (name.find('/') != std::string::npos) {
            return false;
        }
        for (const char* skip : { ".SFD", ".DA", ".DRV", ".PVR" }) {
            if (name.size() > strlen(skip) && name.compare(name.size() - strlen(skip), strlen(skip), skip) == 0) {
                return false;
            }
        }
        return name != "IP.BIN" && name != "2_DP.BIN" && name != "MAIGO.BIN" && name != "SG_DPLDR.BIN";
    }

    bool is_song(const std::string& name) {
        return name.size() > 4 && name.compare(name.size() - 4, 4, ".STR") == 0;
    }

    // .STR (block-interleaved PCM16) to the pack's ADPCM: per disc block, each channel's block_samples samples.
    bool encode_song(const std::vector<uint8_t>& str, std::vector<uint8_t>& out) {
        constexpr size_t header = 0x800;
        if (str.size() < header + 32) {
            return false;
        }
        uint32_t block_bytes = le32(&str[12]), channels = le32(&str[24]);
        if (block_bytes != rush2::rom2049::dc::music_block_samples * 2 || channels != 2) {
            return false;
        }
        size_t blocks = (str.size() - header) / (block_bytes * channels);
        constexpr int n = rush2::rom2049::dc::music_block_samples;
        out.assign(str.begin(), str.begin() + header);
        std::vector<int16_t> pcm(n);
        for (size_t b = 0; b < blocks; b++) {
            for (uint32_t c = 0; c < channels; c++) {
                memcpy(pcm.data(), &str[header + (b * channels + c) * block_bytes], block_bytes);
                size_t at = out.size();
                out.resize(at + 4 + n / 2);
                rush2::rom2049::dc::adpcm_encode(pcm.data(), n, &out[at]);
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Pack

    struct PackEntry {
        uint64_t offset;
        uint64_t size;
        Kind kind;
    };

    class PackFiles : public rush2::rom2049::dc::Files {
    public:
        bool open(const fs::path& path) {
            file.open(path, std::ios::binary);
            char magic[8];
            uint32_t version = 0, count = 0, path_len = 0;
            if (!file.read(magic, 8) || memcmp(magic, pack_magic, 8) != 0 ||
                !file.read((char*)&version, 4) || version != pack_version ||
                !file.read((char*)&count, 4) || !file.read((char*)&path_len, 4) || path_len > 4096) {
                return false;
            }
            image_path.resize(path_len);
            file.read(image_path.data(), path_len);
            for (uint32_t i = 0; i < count && file; i++) {
                uint8_t n = 0;
                file.read((char*)&n, 1);
                std::string name(n, '\0');
                PackEntry e{};
                file.read(name.data(), n);
                file.read((char*)&e.offset, 8);
                file.read((char*)&e.size, 8);
                file.read((char*)&e.kind, 4);
                entries[name] = e;
            }
            return bool(file);
        }

        bool raw(const std::string& name, std::vector<uint8_t>& out) const override {
            std::lock_guard lock{ mutex };
            auto it = entries.find(upper(name));
            if (it == entries.end() || it->second.kind != Kind::Raw) {
                return false;
            }
            return read(it->second, 0, it->second.size, out);
        }

        // Any entry, songs included (in the pack's ADPCM).
        bool stored(const std::string& name, std::vector<uint8_t>& out) const {
            std::lock_guard lock{ mutex };
            auto it = entries.find(upper(name));
            return it != entries.end() && read(it->second, 0, it->second.size, out);
        }

        bool read(const PackEntry& e, uint64_t from, uint64_t size, std::vector<uint8_t>& out) const {
            out.resize((size_t)size);
            file.clear();
            file.seekg((std::streamoff)(e.offset + from));
            return bool(file.read((char*)out.data(), (std::streamsize)size));
        }

        std::string image_path;
        std::map<std::string, PackEntry> entries;
        mutable std::ifstream file;
        mutable std::mutex mutex;
    };

    class DcSource : public rush2::rom2049::Source {
    public:
        DcSource(std::shared_ptr<PackFiles> files, uint64_t key) : files(std::move(files)), key(key) {}

        // The converted files are kept on disk (src/track_cache.cpp): converting a disc model takes long enough to
        // show as a hitch the first time a track or car is used.
        bool read_file(int index, std::vector<uint8_t>& out) const override {
            std::lock_guard lock{ mutex };
            auto it = converted.find(index);
            if (it == converted.end()) {
                auto data = std::make_shared<std::vector<uint8_t>>();
                std::string name = "dc_file_" + std::to_string(index);
                // The textures it scaled down go in a blob of their own.
                std::vector<uint8_t> textures;
                std::vector<rush2::rom2049::SourceTexture> made;
                if (rush2::track_cache::load_blob(name, key, *data) &&
                    rush2::track_cache::load_blob(name + "_textures", key, textures) && read_textures(textures, made)) {
                    shrunk.insert(shrunk.end(), made.begin(), made.end());
                }
                else if (rush2::rom2049::dc::convert_file(*files, index, *data, &made)) {
                    rush2::track_cache::save_blob(name, key, *data);
                    rush2::track_cache::save_blob(name + "_textures", key, write_textures(made));
                    shrunk.insert(shrunk.end(), made.begin(), made.end());
                }
                else {
                    data = nullptr;
                }
                it = converted.emplace(index, data).first;
            }
            if (it->second == nullptr) {
                return false;
            }
            out = *it->second;
            return true;
        }

        std::shared_ptr<const std::vector<uint8_t>> segment(rush2::rom2049::Segment s) const override {
            std::lock_guard lock{ mutex };
            auto& cached = segments[(int)s];
            if (cached == nullptr) {
                std::vector<uint8_t> exe;
                auto data = std::make_shared<std::vector<uint8_t>>();
                if (!files->raw("1ST_READ.BIN", exe) || !rush2::rom2049::dc::build_segment(exe, s, *data)) {
                    return nullptr;
                }
                cached = std::move(data);
            }
            return cached;
        }

        bool is_dreamcast() const override {
            return true;
        }

        bool disc_file(const std::string& name, std::vector<uint8_t>& out) const override {
            return files->stored(name, out);
        }

        size_t source_texture_count() const override {
            std::lock_guard lock{ mutex };
            return shrunk.size();
        }

        std::vector<rush2::rom2049::SourceTexture> source_textures(size_t from) const override {
            std::lock_guard lock{ mutex };
            return from < shrunk.size() ? std::vector<rush2::rom2049::SourceTexture>(shrunk.begin() + from, shrunk.end())
                                        : std::vector<rush2::rom2049::SourceTexture>{};
        }

        // Decodes from the container last asked for (a run of textures mostly comes from one file).
        bool source_image(const rush2::rom2049::SourceTexture& t, std::vector<uint8_t>& rgba, int& w, int& h) const override {
            std::lock_guard lock{ image_mutex };
            if (image_file != t.file) {
                image_container.clear();
                image_file.clear();
                if (!files->get(t.file + ".LZS", image_container)) return false;
                image_file = t.file;
            }
            return rush2::rom2049::dc::decode_texture(image_container, t.index, t.tint, rgba, w, h);
        }

        uint64_t cache_key() const override {
            return key;
        }

        std::shared_ptr<PackFiles> files;

    private:
        uint64_t key;
        mutable std::mutex mutex;
        mutable std::map<int, std::shared_ptr<const std::vector<uint8_t>>> converted;
        mutable std::shared_ptr<const std::vector<uint8_t>> segments[3];
        mutable std::vector<rush2::rom2049::SourceTexture> shrunk;
        mutable std::mutex image_mutex;
        mutable std::string image_file;
        mutable std::vector<uint8_t> image_container;

        // A file's scaled-down textures as cached: per texture u16 w, u16 h, u8 rgba32, u32 tint, u32 index, u8 name
        // length, name, u32 texel bytes, texels.
        static std::vector<uint8_t> write_textures(const std::vector<rush2::rom2049::SourceTexture>& list) {
            std::vector<uint8_t> out;
            auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
            for (const auto& t : list) {
                uint8_t rgba32 = t.rgba32, n = (uint8_t)t.file.size();
                uint32_t bytes = (uint32_t)t.texels.size();
                put(&t.w, 2); put(&t.h, 2); put(&rgba32, 1); put(&t.tint, 4); put(&t.index, 4);
                put(&n, 1); put(t.file.data(), n); put(&bytes, 4); put(t.texels.data(), bytes);
            }
            return out;
        }

        static bool read_textures(const std::vector<uint8_t>& in, std::vector<rush2::rom2049::SourceTexture>& list) {
            size_t o = 0;
            auto get = [&](void* p, size_t n) {
                if (o + n > in.size()) return false;
                memcpy(p, &in[o], n);
                o += n;
                return true;
            };
            while (o < in.size()) {
                rush2::rom2049::SourceTexture t;
                uint8_t rgba32 = 0, n = 0;
                uint32_t bytes = 0;
                if (!get(&t.w, 2) || !get(&t.h, 2) || !get(&rgba32, 1) || !get(&t.tint, 4) || !get(&t.index, 4) ||
                    !get(&n, 1)) {
                    return false;
                }
                t.file.resize(n);
                if (!get(t.file.data(), n) || !get(&bytes, 4) || bytes > in.size()) return false;
                t.texels.resize(bytes);
                if (!get(t.texels.data(), bytes)) return false;
                t.rgba32 = rgba32 != 0;
                list.push_back(std::move(t));
            }
            return true;
        }
    };
}

bool rush2::rom2049::dc::Files::get(const std::string& name, std::vector<uint8_t>& out) const {
    std::vector<uint8_t> stored;
    if (!raw(name, stored)) {
        return false;
    }
    if (name.size() > 4 && upper(name.substr(name.size() - 4)) == ".LZS") {
        return rush2::wings::lz_decompress(stored.data(), stored.size(), out);
    }
    out = std::move(stored);
    return true;
}

bool rush2::rom2049::dc::is_disc_image(const fs::path& path) {
    std::string ext = upper(path.extension().string());
    return ext == ".CDI" || ext == ".GDI" || ext == ".ISO";
}

ImportResult rush2::rom2049::dc::import(const fs::path& image, const fs::path& pack) {
    Disc disc;
    std::map<std::string, Entry> files;
    std::string ext = upper(image.extension().string());
    {
        std::ifstream probe(image, std::ios::binary);
        if (!probe) {
            return ImportResult::FailedToOpen;
        }
    }
    bool found = ext == ".GDI" ? open_gdi(image, disc, files) : open_single_file(image, disc, files);
    if (!found) {
        return ImportResult::WrongGame;
    }
    std::vector<uint8_t> exe;
    if (!read_entry(disc, files["1ST_READ.BIN"], exe)) {
        return ImportResult::FailedToOpen;
    }
    if (!known_executable(exe)) {
        return ImportResult::WrongVersion;
    }

    // Write to a temporary file, then move it over the pack.
    fs::path temp = pack;
    temp += ".tmp";
    std::error_code ec;
    fs::create_directories(pack.parent_path(), ec);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return ImportResult::WriteFailed;
    }
    auto image_u8 = image.u8string();
    std::string image_path(image_u8.begin(), image_u8.end());
    std::vector<std::string> names;
    for (const auto& [name, e] : files) {
        if (wanted(name)) {
            names.push_back(name);
        }
    }
    uint32_t count = (uint32_t)names.size(), path_len = (uint32_t)image_path.size();
    out.write(pack_magic, 8);
    out.write((const char*)&pack_version, 4);
    out.write((const char*)&count, 4);
    out.write((const char*)&path_len, 4);
    out.write(image_path.data(), path_len);
    // Entries are written with placeholder offsets and sizes, then rewritten once the data is in.
    std::vector<std::streamoff> entry_at;
    for (const std::string& name : names) {
        uint8_t n = (uint8_t)name.size();
        out.write((const char*)&n, 1);
        out.write(name.data(), n);
        entry_at.push_back(out.tellp());
        uint64_t zero = 0;
        uint32_t kind = 0;
        out.write((const char*)&zero, 8);
        out.write((const char*)&zero, 8);
        out.write((const char*)&kind, 4);
    }
    std::vector<PackEntry> written;
    std::vector<uint8_t> data, encoded;
    for (const std::string& name : names) {
        if (!read_entry(disc, files[name], data)) {
            out.close();
            fs::remove(temp, ec);
            return ImportResult::FailedToOpen;
        }
        PackEntry e{ (uint64_t)out.tellp(), 0, Kind::Raw };
        const std::vector<uint8_t>* body = &data;
        if (is_song(name) && encode_song(data, encoded)) {
            e.kind = Kind::Music;
            body = &encoded;
        }
        e.size = body->size();
        out.write((const char*)body->data(), (std::streamsize)body->size());
        written.push_back(e);
    }
    for (size_t i = 0; i < names.size(); i++) {
        out.seekp(entry_at[i]);
        out.write((const char*)&written[i].offset, 8);
        out.write((const char*)&written[i].size, 8);
        out.write((const char*)&written[i].kind, 4);
    }
    out.close();
    if (!out) {
        fs::remove(temp, ec);
        return ImportResult::WriteFailed;
    }
    fs::rename(temp, pack, ec);
    if (ec) {
        fs::remove(temp, ec);
        return ImportResult::WriteFailed;
    }
    return ImportResult::Good;
}

std::shared_ptr<const rush2::rom2049::Source> rush2::rom2049::dc::open_pack(const fs::path& pack) {
    auto files = std::make_shared<PackFiles>();
    if (!files->open(pack)) {
        return nullptr;
    }
    std::vector<uint8_t> exe;
    if (!files->raw("1ST_READ.BIN", exe) || !known_executable(exe)) {
        return nullptr;
    }
    // The pack is only ever rewritten whole, so its path, size and time identify what was converted from it.
    std::error_code ec;
    uint64_t size = (uint64_t)fs::file_size(pack, ec);
    uint64_t time = (uint64_t)fs::last_write_time(pack, ec).time_since_epoch().count();
    std::u8string path = pack.u8string();
    uint64_t key = rush2::track_cache::hash((const uint8_t*)path.data(), path.size(), 0xDC);
    key = rush2::track_cache::hash((const uint8_t*)&size, sizeof(size), key);
    key = rush2::track_cache::hash((const uint8_t*)&time, sizeof(time), key);
    return std::make_shared<DcSource>(std::move(files), key == 0 ? 1 : key);
}

// ---------------------------------------------------------------------------------------------------------------
// IMA ADPCM

namespace {
    constexpr int16_t ima_steps[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767,
    };
    constexpr int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

    int ima_step(int predictor, int& index, int nibble) {
        int step = ima_steps[index];
        int diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
        predictor += (nibble & 8) ? -diff : diff;
        index = std::clamp(index + ima_index[nibble], 0, 88);
        return std::clamp(predictor, -32768, 32767);
    }
}

void rush2::rom2049::dc::adpcm_encode(const int16_t* pcm, int count, uint8_t* out) {
    int predictor = pcm[0], index = 0;
    // Start the step size near the block's first difference so the block doesn't open with a ramp.
    if (count > 1) {
        int d = std::abs(pcm[1] - pcm[0]);
        while (index < 88 && ima_steps[index] < d) index++;
    }
    out[0] = uint8_t(predictor);
    out[1] = uint8_t(predictor >> 8);
    out[2] = uint8_t(index);
    out[3] = 0;
    uint8_t* data = out + 4;
    for (int i = 0; i < count; i++) {
        int diff = pcm[i] - predictor;
        int step = ima_steps[index];
        int nibble = 0;
        if (diff < 0) {
            nibble = 8;
            diff = -diff;
        }
        if (diff >= step) { nibble |= 4; diff -= step; }
        if (diff >= step >> 1) { nibble |= 2; diff -= step >> 1; }
        if (diff >= step >> 2) { nibble |= 1; }
        predictor = ima_step(predictor, index, nibble);
        if (i & 1) {
            data[i >> 1] |= uint8_t(nibble << 4);
        }
        else {
            data[i >> 1] = uint8_t(nibble);
        }
    }
}

void rush2::rom2049::dc::adpcm_decode(const uint8_t* in, int count, int16_t* pcm) {
    int predictor = int16_t(in[0] | (in[1] << 8)), index = std::min<int>(in[2], 88);
    const uint8_t* data = in + 4;
    for (int i = 0; i < count; i++) {
        int nibble = (i & 1) ? data[i >> 1] >> 4 : data[i >> 1] & 0xF;
        predictor = ima_step(predictor, index, nibble);
        pcm[i] = int16_t(predictor);
    }
}
