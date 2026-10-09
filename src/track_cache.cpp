// Converted track cache (include/track_cache.h).
//
// File layout: magic "R2TC", format version, the key (kind, track, slot prefix, source hash, build stamp), then the
// converted track's fields in a fixed order, each vector as a u32 count and its elements. Little-endian, as written by
// the host; the file is only ever read back by the build that wrote it.
//
// The build stamp is the executable's size and modification time, so every new build converts again. That keeps a
// cached track from outliving a change to the converters, the ROM readers they use, or the track structs below; the
// static_asserts on the structs' sizes are there to catch a field added without being serialized.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <type_traits>

#include "util/file.h"

#include "track_cache.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    constexpr char magic[4] = { 'R', '2', 'T', 'C' };
    constexpr uint32_t format_version = 1;
    enum class Kind : uint32_t { Rush1 = 1, Rush2049 = 2, Blob = 3 };

    // A field added to a converted track struct has to be added to write()/read() below; then update these sizes.
    static_assert(sizeof(rush2::track1::ConvertedTrack) == 392, "update track_cache.cpp for ConvertedTrack (track1)");
    static_assert(sizeof(rush2::track2049::ConvertedTrack) == 392, "update track_cache.cpp for ConvertedTrack (2049)");
    static_assert(sizeof(rush2::track1::ConvertedTrack::Timing) == 64, "update track_cache.cpp for Timing");
    static_assert(sizeof(rush2::track2049::TexFlipbook) == 72, "update track_cache.cpp for TexFlipbook");
    static_assert(sizeof(rush2::track2049::TexScroll) == 72, "update track_cache.cpp for TexScroll");

    std::filesystem::path cache_dir() {
        return recompui::file::get_app_folder_path() / "track_cache";
    }

    uint64_t build_stamp() {
        static uint64_t stamp = [] {
            std::filesystem::path exe;
#ifdef _WIN32
            wchar_t buf[MAX_PATH * 4];
            DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
            if (n > 0 && n < std::size(buf)) exe = std::filesystem::path(std::wstring(buf, n));
#else
            std::error_code ec;
            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
            std::error_code ec;
            uint64_t size = exe.empty() ? 0 : (uint64_t)std::filesystem::file_size(exe, ec);
            uint64_t time = exe.empty() ? 0 : (uint64_t)std::filesystem::last_write_time(exe, ec).time_since_epoch().count();
            return rush2::track_cache::hash((const uint8_t*)&time, sizeof(time),
                                            rush2::track_cache::hash((const uint8_t*)&size, sizeof(size)));
        }();
        return stamp;
    }

    class Writer {
    public:
        std::vector<uint8_t> bytes;

        template <typename T> void pod(const T& v) {
            static_assert(std::is_trivially_copyable_v<T>);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            bytes.insert(bytes.end(), p, p + sizeof(T));
        }
        template <typename T> void pods(const std::vector<T>& v) {
            static_assert(std::is_trivially_copyable_v<T>);
            pod((uint32_t)v.size());
            const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
            bytes.insert(bytes.end(), p, p + v.size() * sizeof(T));
        }
        void str(const std::string& s) {
            pod((uint32_t)s.size());
            bytes.insert(bytes.end(), s.begin(), s.end());
        }
        void strmap(const std::map<std::string, std::string>& m) {
            pod((uint32_t)m.size());
            for (const auto& [k, v] : m) {
                str(k);
                str(v);
            }
        }
    };

    class Reader {
    public:
        const std::vector<uint8_t>& bytes;
        size_t at = 0;
        bool ok = true;

        explicit Reader(const std::vector<uint8_t>& b) : bytes(b) {}

        bool take(void* out, size_t n) {
            if (!ok || n > bytes.size() - at) {
                ok = false;
                return false;
            }
            memcpy(out, bytes.data() + at, n);
            at += n;
            return true;
        }
        template <typename T> void pod(T& v) {
            static_assert(std::is_trivially_copyable_v<T>);
            take(&v, sizeof(T));
        }
        uint32_t count(size_t element_size) {
            uint32_t n = 0;
            pod(n);
            if (ok && (uint64_t)n * element_size > bytes.size() - at) ok = false;
            return ok ? n : 0;
        }
        template <typename T> void pods(std::vector<T>& v) {
            static_assert(std::is_trivially_copyable_v<T>);
            v.resize(count(sizeof(T)));
            take(v.data(), v.size() * sizeof(T));
        }
        void str(std::string& s) {
            s.resize(count(1));
            take(s.data(), s.size());
        }
        void strmap(std::map<std::string, std::string>& m) {
            m.clear();
            uint32_t n = count(8);
            for (uint32_t i = 0; ok && i < n; i++) {
                std::string k, v;
                str(k);
                str(v);
                m[k] = v;
            }
        }
    };

    void key(Writer& w, Kind kind, int track, const std::string& prefix, uint64_t source) {
        w.bytes.insert(w.bytes.end(), magic, magic + 4);
        w.pod(format_version);
        w.pod(kind);
        w.pod((int32_t)track);
        w.str(prefix);
        w.pod(source);
        w.pod(build_stamp());
    }

    std::filesystem::path file_for(Kind kind, int track) {
        return cache_dir() / ((kind == Kind::Rush1 ? "rush1_" : "rush2049_") + std::to_string(track) + ".bin");
    }

    // Reads the entry's file and checks its key; the reader is then at the track's fields.
    bool open(Kind kind, int track, const std::string& prefix, uint64_t source, std::vector<uint8_t>& bytes) {
        std::ifstream in(file_for(kind, track), std::ios::binary);
        if (!in) return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        Writer expected;
        key(expected, kind, track, prefix, source);
        return bytes.size() >= expected.bytes.size() &&
               memcmp(bytes.data(), expected.bytes.data(), expected.bytes.size()) == 0;
    }

    size_t key_size(Kind kind, int track, const std::string& prefix, uint64_t source) {
        Writer w;
        key(w, kind, track, prefix, source);
        return w.bytes.size();
    }

    // Written to a temporary file and renamed, so a crash never leaves half an entry behind.
    void store(Kind kind, int track, const std::vector<uint8_t>& bytes) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir(), ec);
        std::filesystem::path path = file_for(kind, track);
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            if (!out) return;
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            fprintf(stderr, "[TrackCache] Couldn't write %s: %s\n", path.string().c_str(), ec.message().c_str());
        }
    }

    void write(Writer& w, const rush2::track1::ConvertedTrack& t) {
        w.pods(t.geometry);
        w.pods(t.placement);
        for (const auto& c : t.collision) w.pods(c);
        for (const auto& p : t.path) w.pods(p);
        w.pods(t.pvs);
        w.pod(t.pvs_count);
        for (const auto& d : t.demo_starts) w.pods(d);
        for (float s : t.lap_seconds) w.pod(s);
        for (const auto& timing : t.timing) {
            w.pod(timing.start);
            w.pods(timing.checkpoints);
            w.pod(timing.no_wrong_way);
            w.pods(timing.radius2);
        }
        w.strmap(t.record_models);
        w.strmap(t.piece_models);
    }

    void read(Reader& r, rush2::track1::ConvertedTrack& t) {
        r.pods(t.geometry);
        r.pods(t.placement);
        for (auto& c : t.collision) r.pods(c);
        for (auto& p : t.path) r.pods(p);
        r.pods(t.pvs);
        r.pod(t.pvs_count);
        for (auto& d : t.demo_starts) r.pods(d);
        for (float& s : t.lap_seconds) r.pod(s);
        for (auto& timing : t.timing) {
            r.pod(timing.start);
            r.pods(timing.checkpoints);
            r.pod(timing.no_wrong_way);
            r.pods(timing.radius2);
        }
        r.strmap(t.record_models);
        r.strmap(t.piece_models);
    }

    void write(Writer& w, const rush2::track2049::ConvertedTrack& t) {
        w.pods(t.geometry);
        w.pods(t.placement);
        w.pods(t.collision);
        w.pods(t.path);
        w.pods(t.path_backward);
        w.pods(t.pvs);
        w.pod(t.fog);
        w.pod(t.pvs_count);
        w.pods(t.path_records);
        w.pods(t.spin_records);
        w.pods(t.prop_records);
        w.pods(t.pickup_records);
        w.pods(t.pool_records);
        w.pods(t.solid_triangles);
        for (const auto& d : t.demo_starts) w.pods(d);
        w.pod((uint32_t)t.tex_anims.flipbooks.size());
        for (const auto& f : t.tex_anims.flipbooks) {
            w.str(f.target);
            w.pod(f.settimg);
            w.pods(f.frames);
            w.pod(f.start);
            w.pod(f.forward);
            w.pod(f.period);
        }
        w.pod((uint32_t)t.tex_anims.scrolls.size());
        for (const auto& s : t.tex_anims.scrolls) {
            w.str(s.target);
            w.pods(s.tile_sizes);
            w.pod(s.t);
            w.pod(s.position);
            w.pod(s.wrap);
            w.pod(s.speed);
            w.pod(s.rate);
        }
    }

    void read(Reader& r, rush2::track2049::ConvertedTrack& t) {
        r.pods(t.geometry);
        r.pods(t.placement);
        r.pods(t.collision);
        r.pods(t.path);
        r.pods(t.path_backward);
        r.pods(t.pvs);
        r.pod(t.fog);
        r.pod(t.pvs_count);
        r.pods(t.path_records);
        r.pods(t.spin_records);
        r.pods(t.prop_records);
        r.pods(t.pickup_records);
        r.pods(t.pool_records);
        r.pods(t.solid_triangles);
        for (auto& d : t.demo_starts) r.pods(d);
        t.tex_anims.flipbooks.resize(r.count(1));
        for (auto& f : t.tex_anims.flipbooks) {
            r.str(f.target);
            r.pod(f.settimg);
            r.pods(f.frames);
            r.pod(f.start);
            r.pod(f.forward);
            r.pod(f.period);
        }
        t.tex_anims.scrolls.resize(r.count(1));
        for (auto& s : t.tex_anims.scrolls) {
            r.str(s.target);
            r.pods(s.tile_sizes);
            r.pod(s.t);
            r.pod(s.position);
            r.pod(s.wrap);
            r.pod(s.speed);
            r.pod(s.rate);
        }
    }

    std::mutex cache_mutex;

    std::filesystem::path blob_file(const std::string& name) {
        return cache_dir() / ("blob_" + name + ".bin");
    }
}

// FNV-1a over 8-byte words (then the tail bytes), with a rotate so high bits feed back: a whole ROM hashes in a few
// milliseconds, which matters because the first race of a session hashes its ROM on the race's first frame.
std::filesystem::path rush2::track_cache::directory() {
    return cache_dir();
}

uint64_t rush2::track_cache::build() {
    return build_stamp();
}

uint64_t rush2::track_cache::hash(const uint8_t* data, size_t size, uint64_t seed) {
    constexpr uint64_t prime = 0x100000001B3ull;
    uint64_t h = seed;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, 8);
        h = (h ^ word) * prime;
        h ^= h >> 29;
    }
    for (; i < size; i++) {
        h = (h ^ data[i]) * prime;
    }
    return h;
}

bool rush2::track_cache::load_rush1(int t, const std::string& prefix, uint64_t source, rush2::track1::ConvertedTrack& track,
                                    std::vector<uint8_t>& logo) {
    std::lock_guard lock{ cache_mutex };
    std::vector<uint8_t> bytes;
    if (!open(Kind::Rush1, t, prefix, source, bytes)) return false;
    Reader r(bytes);
    r.at = key_size(Kind::Rush1, t, prefix, source);
    rush2::track1::ConvertedTrack loaded;
    read(r, loaded);
    r.pods(logo);
    if (!r.ok || r.at != bytes.size()) return false;
    track = std::move(loaded);
    return true;
}

void rush2::track_cache::save_rush1(int t, const std::string& prefix, uint64_t source, const rush2::track1::ConvertedTrack& track,
                                    const std::vector<uint8_t>& logo) {
    std::lock_guard lock{ cache_mutex };
    Writer w;
    key(w, Kind::Rush1, t, prefix, source);
    write(w, track);
    w.pods(logo);
    store(Kind::Rush1, t, w.bytes);
}

bool rush2::track_cache::load_2049(int k, const std::string& prefix, uint64_t source, rush2::track2049::ConvertedTrack& track) {
    std::lock_guard lock{ cache_mutex };
    std::vector<uint8_t> bytes;
    if (!open(Kind::Rush2049, k, prefix, source, bytes)) return false;
    Reader r(bytes);
    r.at = key_size(Kind::Rush2049, k, prefix, source);
    rush2::track2049::ConvertedTrack loaded;
    read(r, loaded);
    if (!r.ok || r.at != bytes.size()) return false;
    track = std::move(loaded);
    return true;
}

void rush2::track_cache::save_2049(int k, const std::string& prefix, uint64_t source, const rush2::track2049::ConvertedTrack& track) {
    std::lock_guard lock{ cache_mutex };
    Writer w;
    key(w, Kind::Rush2049, k, prefix, source);
    write(w, track);
    store(Kind::Rush2049, k, w.bytes);
}

bool rush2::track_cache::load_blob(const std::string& name, uint64_t source, std::vector<uint8_t>& out) {
    if (source == 0) return false;
    std::lock_guard lock{ cache_mutex };
    std::ifstream in(blob_file(name), std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Writer expected;
    key(expected, Kind::Blob, 0, name, source);
    size_t at = expected.bytes.size();
    if (bytes.size() < at || memcmp(bytes.data(), expected.bytes.data(), at) != 0) return false;
    Reader r(bytes);
    r.at = at;
    std::vector<uint8_t> loaded;
    r.pods(loaded);
    if (!r.ok || r.at != bytes.size()) return false;
    out = std::move(loaded);
    return true;
}

void rush2::track_cache::save_blob(const std::string& name, uint64_t source, const std::vector<uint8_t>& data) {
    if (source == 0) return;
    std::lock_guard lock{ cache_mutex };
    Writer w;
    key(w, Kind::Blob, 0, name, source);
    w.pods(data);
    std::error_code ec;
    std::filesystem::create_directories(cache_dir(), ec);
    std::filesystem::path path = blob_file(name);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(w.bytes.data()), (std::streamsize)w.bytes.size());
        if (!out) return;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        fprintf(stderr, "[TrackCache] Couldn't write %s: %s\n", path.string().c_str(), ec.message().c_str());
    }
}
