// Converted track cache (include/track_cache.h).
//
// File layout: magic "R2TC", format version, the key (kind, track, slot prefix, source hash, build stamp), then the
// converted track's fields in a fixed order, each vector as a u32 count and its elements. Little-endian, as written by
// the host; the file is only ever read back by the build that wrote it.
//
// The build stamp is the executable's size and modification time, so every new build converts again. That keeps a
// cached track from outliving a change to the converters, the ROM readers they use, or the track structs below; the
// static_asserts on the structs' sizes are there to catch a field added without being serialized.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "librecomp/game.hpp"
#include "util/file.h"

#include "track_cache.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    constexpr char magic[4] = { 'R', '2', 'T', 'C' };
    constexpr uint32_t format_version = 3;
    enum class Kind : uint32_t { Rush1 = 1, Rush2049 = 2, Blob = 3 };

    // A field added to a converted track struct has to be added to write()/read() below; then update these sizes (as
    // MSVC lays the structs out; other standard libraries size their members differently).
#ifdef _MSC_VER
    static_assert(sizeof(rush2::track1::ConvertedTrack) == 392, "update track_cache.cpp for ConvertedTrack (track1)");
    static_assert(sizeof(rush2::track2049::ConvertedTrack) == 512, "update track_cache.cpp for ConvertedTrack (2049)");
    static_assert(sizeof(rush2::track1::ConvertedTrack::Timing) == 64, "update track_cache.cpp for Timing");
    static_assert(sizeof(rush2::track2049::TexFlipbook) == 72, "update track_cache.cpp for TexFlipbook");
    static_assert(sizeof(rush2::track2049::TexScroll) == 72, "update track_cache.cpp for TexScroll");
    static_assert(sizeof(rush2::track2049::TexPalette) == 72, "update track_cache.cpp for TexPalette");
#endif

    std::filesystem::path cache_dir() {
        return recomp::get_config_path() / "track_cache";
    }

    uint64_t build_stamp() {
        static uint64_t stamp = [] {
            std::filesystem::path exe;
#ifdef _WIN32
            wchar_t buf[MAX_PATH * 4];
            DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
            if (n > 0 && n < std::size(buf)) exe = std::filesystem::path(std::wstring(buf, n));
#endif
            std::error_code ec;
#ifndef _WIN32
            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
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

    // Entries read ahead into memory (rush2::track_cache::preload_*), by file: the next load of one takes its bytes
    // from here instead of the disk, once. At most preload_limit bytes, the oldest dropped first (a track picked at the
    // car select and then left unraced stays until then).
    constexpr size_t preload_limit = 64u << 20;
    std::mutex preloaded_mutex;
    std::unordered_map<std::string, std::vector<uint8_t>> preloaded;
    std::vector<std::string> preload_order;
    size_t preloaded_bytes = 0;

    std::string path_key(const std::filesystem::path& path) {
        std::u8string u = path.generic_u8string();
        return std::string(u.begin(), u.end());
    }

    // The bytes preloaded for the file, taken out (false if there are none).
    bool take_preloaded(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
        std::lock_guard lock{ preloaded_mutex };
        auto it = preloaded.find(path_key(path));
        if (it == preloaded.end()) return false;
        bytes = std::move(it->second);
        preloaded_bytes -= bytes.size();
        preload_order.erase(std::find(preload_order.begin(), preload_order.end(), it->first));
        preloaded.erase(it);
        return true;
    }

    // The whole file in one read (a character iterator took milliseconds for a car texture's upscale).
    bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        bytes.resize(size_t(in.tellg()));
        in.seekg(0);
        return (bool)in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size());
    }

    // The file's bytes if they start with `expected` (the entry's key): the preloaded ones, or else (none, or written
    // again since they were read) the disk's.
    bool read_entry(const std::filesystem::path& path, const std::vector<uint8_t>& expected, std::vector<uint8_t>& bytes) {
        auto keyed = [&] {
            return bytes.size() >= expected.size() && memcmp(bytes.data(), expected.data(), expected.size()) == 0;
        };
        if (take_preloaded(path, bytes) && keyed()) return true;
        return read_file(path, bytes) && keyed();
    }

    // Whether the file starts with `expected` (an entry's key), reading only that much.
    bool starts_with(const std::filesystem::path& path, const std::vector<uint8_t>& expected) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::vector<uint8_t> head(expected.size());
        return in.read(reinterpret_cast<char*>(head.data()), (std::streamsize)head.size()) && head == expected;
    }

    // Reads the entry's file and checks its key; the reader is then at the track's fields.
    bool open(Kind kind, int track, const std::string& prefix, uint64_t source, std::vector<uint8_t>& bytes) {
        Writer expected;
        key(expected, kind, track, prefix, source);
        return read_entry(file_for(kind, track), expected.bytes, bytes);
    }

    size_t key_size(Kind kind, int track, const std::string& prefix, uint64_t source) {
        Writer w;
        key(w, kind, track, prefix, source);
        return w.bytes.size();
    }

    // Written to a temporary file of the writing thread's own and renamed, so a crash never leaves half an entry
    // behind and two threads writing at once (a background build and the game) never share one.
    void store(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir(), ec);
        std::filesystem::path temp = path;
        temp += "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000000) + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            if (!out) return;
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            fprintf(stderr, "[TrackCache] Couldn't write %s: %s\n", path.string().c_str(), ec.message().c_str());
            std::filesystem::remove(temp, ec);
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
        w.pod(t.sky_scale);
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
        w.pod((uint32_t)t.tex_anims.palettes.size());
        for (const auto& p : t.tex_anims.palettes) {
            w.str(p.target);
            w.pod(p.data);
            w.pods(p.colors);
            w.pod(p.first);
            w.pod(p.last);
            w.pod(p.step);
            w.pod(p.kind);
            w.pod(p.period);
        }
        for (const auto& b : t.tex_anims.blink) w.pods(b);
        w.pods(t.mover_paths);
        w.pods(t.mover_collision);
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
        r.pod(t.sky_scale);
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
        t.tex_anims.palettes.resize(r.count(1));
        for (auto& p : t.tex_anims.palettes) {
            r.str(p.target);
            r.pod(p.data);
            r.pods(p.colors);
            r.pod(p.first);
            r.pod(p.last);
            r.pod(p.step);
            r.pod(p.kind);
            r.pod(p.period);
        }
        for (auto& b : t.tex_anims.blink) r.pods(b);
        r.pods(t.mover_paths);
        r.pods(t.mover_collision);
    }

    std::filesystem::path blob_file(const std::string& name) {
        return cache_dir() / ("blob_" + name + ".bin");
    }

    // One mutex per entry for rush2::track_cache::claim, never freed (a few hundred entries at most).
    std::mutex claims_mutex;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> claims;

    void preload_file(const std::filesystem::path& path) {
        std::string key = path_key(path);
        {
            std::lock_guard lock{ preloaded_mutex };
            if (preloaded.count(key)) return;
        }
        std::vector<uint8_t> bytes;
        if (!read_file(path, bytes) || bytes.size() > preload_limit) return;
        std::lock_guard lock{ preloaded_mutex };
        if (preloaded.count(key)) return;
        while (preloaded_bytes + bytes.size() > preload_limit && !preload_order.empty()) {
            auto it = preloaded.find(preload_order.front());
            preloaded_bytes -= it->second.size();
            preloaded.erase(it);
            preload_order.erase(preload_order.begin());
        }
        preloaded_bytes += bytes.size();
        preload_order.push_back(key);
        preloaded.emplace(key, std::move(bytes));
    }
}

std::unique_lock<std::mutex> rush2::track_cache::claim(const std::string& entry) {
    std::mutex* m;
    {
        std::lock_guard lock{ claims_mutex };
        std::unique_ptr<std::mutex>& p = claims[entry];
        if (!p) p = std::make_unique<std::mutex>();
        m = p.get();
    }
    return std::unique_lock<std::mutex>(*m);
}

std::string rush2::track_cache::rush1_entry(int t) {
    return "rush1_" + std::to_string(t);
}

std::string rush2::track_cache::entry_2049(int k) {
    return "rush2049_" + std::to_string(k);
}

std::string rush2::track_cache::blob_entry(const std::string& name) {
    return "blob_" + name;
}

namespace {
    // Lookups that found no entry for this build: something is about to be converted (misses()).
    std::atomic<uint64_t> miss_count = 0;

    bool found(bool hit) {
        if (!hit) miss_count++;
        return hit;
    }
}

uint64_t rush2::track_cache::misses() {
    return miss_count;
}

bool rush2::track_cache::has_rush1(int t, const std::string& prefix, uint64_t source) {
    Writer expected;
    key(expected, Kind::Rush1, t, prefix, source);
    return found(starts_with(file_for(Kind::Rush1, t), expected.bytes));
}

bool rush2::track_cache::has_2049(int k, const std::string& prefix, uint64_t source) {
    Writer expected;
    key(expected, Kind::Rush2049, k, prefix, source);
    return found(starts_with(file_for(Kind::Rush2049, k), expected.bytes));
}

bool rush2::track_cache::has_blob(const std::string& name, uint64_t source) {
    if (source == 0) return false;
    Writer expected;
    key(expected, Kind::Blob, 0, name, source);
    return found(starts_with(blob_file(name), expected.bytes));
}

void rush2::track_cache::preload_rush1(int t) {
    preload_file(file_for(Kind::Rush1, t));
}

void rush2::track_cache::preload_2049(int k) {
    preload_file(file_for(Kind::Rush2049, k));
}

void rush2::track_cache::preload_blob(const std::string& name) {
    preload_file(blob_file(name));
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
    std::vector<uint8_t> bytes;
    if (!found(open(Kind::Rush1, t, prefix, source, bytes))) return false;
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
    Writer w;
    key(w, Kind::Rush1, t, prefix, source);
    write(w, track);
    w.pods(logo);
    store(file_for(Kind::Rush1, t), w.bytes);
}

bool rush2::track_cache::load_2049(int k, const std::string& prefix, uint64_t source, rush2::track2049::ConvertedTrack& track) {
    std::vector<uint8_t> bytes;
    if (!found(open(Kind::Rush2049, k, prefix, source, bytes))) return false;
    Reader r(bytes);
    r.at = key_size(Kind::Rush2049, k, prefix, source);
    rush2::track2049::ConvertedTrack loaded;
    read(r, loaded);
    if (!r.ok || r.at != bytes.size()) return false;
    track = std::move(loaded);
    return true;
}

void rush2::track_cache::save_2049(int k, const std::string& prefix, uint64_t source, const rush2::track2049::ConvertedTrack& track) {
    Writer w;
    key(w, Kind::Rush2049, k, prefix, source);
    write(w, track);
    store(file_for(Kind::Rush2049, k), w.bytes);
}

bool rush2::track_cache::load_blob(const std::string& name, uint64_t source, std::vector<uint8_t>& out) {
    if (source == 0) return false;
    Writer expected;
    key(expected, Kind::Blob, 0, name, source);
    size_t at = expected.bytes.size();
    std::vector<uint8_t> bytes;
    if (!found(read_entry(blob_file(name), expected.bytes, bytes))) return false;
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
    Writer w;
    key(w, Kind::Blob, 0, name, source);
    w.pods(data);
    store(blob_file(name), w.bytes);
}
