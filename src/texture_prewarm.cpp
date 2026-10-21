// Texture prewarming (include/texture_prewarm.h, docs/hitch_profiler.md#race-start-textures).
//
// RT64 uploads a texture (copies TMEM to the GPU and decodes it there) the first time the game draws with it, and the
// frame waits for the upload. A race's first frames bring hundreds of textures at once: the frame that starts the
// race intro waited about 20 ms for them. Here, the textures a race uploads from its load until a few seconds into
// the racing are recorded (RT64::TextureUploadObserver: the texture's hash, TMEM and tile) and kept on disk per
// track (<config>/shader_cache/race_textures_<track>.bin). The next time that track is picked, its list is read on a thread of its own
// and handed to RT64 (RT64::prewarmTexture), which uploads it in small batches behind the game's own uploads while the
// car select is up. The race then finds them in the texture cache and uploads nothing for them.
//
// The race also draws textures made new every frame (render-to-texture effects); RT64 evicts a texture used for a
// single frame 8 frames later, so each recorded texture is checked check_after frames after its upload and kept only if
// the cache still holds it. Checked then rather than when the window ends: a texture the race intro drew stays in the
// cache only up to about 4 s after its last use, so by the end of the window most of the intro's would be gone.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "miniz.h"

#include "common/rt64_live_textures.h"
#include "shared/rt64_f3d_defines.h"   // what rt64_tmem_hasher.h needs before it, as RT64 includes it
#include "xxHash/xxh3.h"
#include "common/rt64_tmem_hasher.h"
#include "recomp.h"

#include "librecomp/game.hpp"
#include "texture_prewarm.h"

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;    // s32: 2 the car select, 3 racing, 7 race start, 9 countdown, 10 intro
    constexpr uint32_t track_id = 0x8010C3F0;      // s8: the track select's id until the race setup
    constexpr uint32_t race_game_mode = 0x800FAE6C; // u8: nonzero while the attract demo runs
    constexpr uint32_t unlock_tracks = 0x800E7D19; // u8: nonzero once ATARI is unlocked
    constexpr uint32_t file_magic = 0x58543252;    // "R2TX"
    // 2: the header holds file_layout, so a list RT64 would read differently (another LoadTile layout or texture hash)
    // is dropped and recorded again instead of being uploaded wrong under a hash the game would match.
    constexpr uint32_t file_version = 2;
    constexpr uint32_t file_layout = uint32_t(sizeof(RT64::LoadTile)) | (RT64::TMEMHasher::CurrentHashVersion << 16);
    constexpr size_t tmem_bytes = 4096;
    constexpr size_t max_textures = 3000;          // per track
    constexpr double record_after_racing = 4.0;    // seconds of racing recorded after the race intro
    constexpr uint32_t check_after = 12;           // game frames from a texture's upload to checking it is still cached

    struct Record {
        uint64_t hash = 0;
        uint16_t width = 0, height = 0;
        uint32_t tlut = 0;
        uint8_t decode = 0;
        RT64::LoadTile tile{};
        std::vector<uint8_t> tmem;
        uint32_t frame = 0;                         // frame_count at its upload (not saved)
    };

    std::mutex mutex;
    int track = -1;                     // the next race's track select id
    bool recording = false;
    std::unordered_set<uint64_t> known; // on the track's list, or recorded
    std::vector<Record> recorded;       // since the list was read, not checked yet
    std::vector<Record> kept;           // recorded and still cached check_after frames later
    std::atomic<uint32_t> frame_count = 0;
    std::atomic<bool> busy = false;     // a list being read or written
    std::atomic<int> prewarmed = -1;    // the track whose list was handed to RT64 last
    std::atomic<bool> raced = false;    // a race was recorded since then (its textures may have aged out)
    std::atomic<bool> picked = false;   // a player's race is set up (set_track): the attract demo's track waits

    // Kept with RT64's shader cache, not keyed on the build: a texture's hash is of its contents, so an entry another
    // build's data no longer has costs one unused upload, and a new build keeps every track's race start smooth.
    std::filesystem::path list_file(int t) {
        return recomp::get_config_path() / "shader_cache" / ("race_textures_" + std::to_string(t) + ".bin");
    }

    void put(std::vector<uint8_t>& out, const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }

    bool get(const std::vector<uint8_t>& in, size_t& at, void* p, size_t n) {
        if (at + n > in.size()) return false;
        memcpy(p, in.data() + at, n);
        at += n;
        return true;
    }

    std::vector<uint8_t> serialize(const std::vector<Record>& records) {
        std::vector<uint8_t> raw;
        raw.reserve(records.size() * (tmem_bytes + 48));
        for (const Record& r : records) {
            put(raw, &r.hash, sizeof(r.hash));
            put(raw, &r.width, sizeof(r.width));
            put(raw, &r.height, sizeof(r.height));
            put(raw, &r.tlut, sizeof(r.tlut));
            put(raw, &r.decode, sizeof(r.decode));
            put(raw, &r.tile, sizeof(r.tile));
            put(raw, r.tmem.data(), tmem_bytes);
        }
        constexpr size_t header = 20;
        mz_ulong packed_size = mz_compressBound((mz_ulong)raw.size());
        std::vector<uint8_t> out(header + packed_size);
        uint32_t count = (uint32_t)records.size(), raw_size = (uint32_t)raw.size();
        memcpy(out.data(), &file_magic, 4);
        memcpy(out.data() + 4, &file_version, 4);
        memcpy(out.data() + 8, &file_layout, 4);
        memcpy(out.data() + 12, &count, 4);
        memcpy(out.data() + 16, &raw_size, 4);
        if (mz_compress2(out.data() + header, &packed_size, raw.data(), (mz_ulong)raw.size(), 1) != MZ_OK) return {};
        out.resize(header + packed_size);
        return out;
    }

    std::vector<Record> deserialize(const std::vector<uint8_t>& in) {
        std::vector<Record> records;
        uint32_t magic = 0, version = 0, layout = 0, count = 0, raw_size = 0;
        size_t at = 0;
        if (!get(in, at, &magic, 4) || !get(in, at, &version, 4) || !get(in, at, &layout, 4) || !get(in, at, &count, 4) ||
            !get(in, at, &raw_size, 4) || magic != file_magic || version != file_version || layout != file_layout ||
            count > max_textures) {
            return records;
        }
        std::vector<uint8_t> raw(raw_size);
        mz_ulong size = raw_size;
        if (mz_uncompress(raw.data(), &size, in.data() + at, (mz_ulong)(in.size() - at)) != MZ_OK || size != raw_size) {
            return records;
        }
        at = 0;
        for (uint32_t i = 0; i < count; i++) {
            Record r;
            r.tmem.resize(tmem_bytes);
            if (!get(raw, at, &r.hash, sizeof(r.hash)) || !get(raw, at, &r.width, sizeof(r.width)) ||
                !get(raw, at, &r.height, sizeof(r.height)) || !get(raw, at, &r.tlut, sizeof(r.tlut)) ||
                !get(raw, at, &r.decode, sizeof(r.decode)) || !get(raw, at, &r.tile, sizeof(r.tile)) ||
                !get(raw, at, r.tmem.data(), tmem_bytes)) {
                records.clear();
                break;
            }
            records.push_back(std::move(r));
        }
        return records;
    }

    std::vector<Record> read_list(int t) {
        std::ifstream f(list_file(t), std::ios::binary);
        if (!f) return {};
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return deserialize(blob);
    }

    void write_list(int t, const std::vector<uint8_t>& blob) {
        std::filesystem::path path = list_file(t), temp = path;
        temp += ".tmp";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(blob.data()), (std::streamsize)blob.size());
            if (!f) return;
        }
        std::filesystem::rename(temp, path, ec);
    }

    // Records the race's new uploads (RT64 calls it on the thread that processes display lists).
    struct Recorder : RT64::TextureUploadObserver {
        void textureUploaded(uint64_t hash, const uint8_t* TMEM, uint16_t width, uint16_t height, uint32_t tlut,
                             const RT64::LoadTile& tile, bool decodeTMEM) override {
            std::lock_guard lock{ mutex };
            if (!recording || known.size() >= max_textures || !known.insert(hash).second) return;
            Record r;
            r.hash = hash;
            r.width = width;
            r.height = height;
            r.tlut = tlut;
            r.decode = decodeTMEM ? 1 : 0;
            r.tile = tile;
            r.tmem.assign(TMEM, TMEM + tmem_bytes);
            r.frame = frame_count;
            recorded.push_back(std::move(r));
        }
    } recorder;

    // Adds what the race recorded (and RT64 held check_after frames later, sift) to the track's list on disk.
    void save(int t, std::vector<Record> records) {
        std::thread([t, records = std::move(records)]() mutable {
            while (busy.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::vector<Record> list = read_list(t);
            std::unordered_set<uint64_t> on_list;
            for (const Record& r : list) on_list.insert(r.hash);
            size_t added = 0;
            for (Record& r : records) {
                if (list.size() >= max_textures) break;
                if (on_list.count(r.hash)) continue;
                on_list.insert(r.hash);
                list.push_back(std::move(r));
                added++;
            }
            if (added > 0) {
                std::vector<uint8_t> blob = serialize(list);
                if (!blob.empty()) write_list(t, blob);
                printf("[texprewarm] track %d: %zu textures kept for its race start (%zu new)\n", t, list.size(), added);
            }
            busy = false;
        }).detach();
    }

    // Checks the recorded textures uploaded at least check_after frames ago: the ones RT64 still holds are kept, the
    // others were made new for a frame or two (render-to-texture effects) and are dropped.
    void sift() {
        std::vector<Record> due;
        {
            std::lock_guard lock{ mutex };
            uint32_t now = frame_count;
            auto aged = std::stable_partition(recorded.begin(), recorded.end(), [&](const Record& r) {
                return now - r.frame < check_after;
            });
            due.assign(std::make_move_iterator(aged), std::make_move_iterator(recorded.end()));
            recorded.erase(aged, recorded.end());
        }
        if (due.empty()) return;
        // RT64's lock, taken without this file's: the recorder takes this one on RT64's display list thread.
        std::vector<Record> cached;
        for (Record& r : due) {
            if (RT64::hasCachedTexture(r.hash)) cached.push_back(std::move(r));
        }
        std::lock_guard lock{ mutex };
        kept.insert(kept.end(), std::make_move_iterator(cached.begin()), std::make_move_iterator(cached.end()));
    }

    // Ends the recording window: what it recorded and kept goes to the track's list. The textures uploaded in its last
    // check_after frames can't be told from a render-to-texture effect yet and are dropped (a texture still in use is
    // recorded by a later race, whose window ends elsewhere on the track).
    void finish() {
        {
            std::lock_guard lock{ mutex };
            recording = false;
        }
        sift();
        std::vector<Record> records;
        int t;
        size_t unchecked = 0;
        {
            std::lock_guard lock{ mutex };
            records.swap(kept);
            unchecked = recorded.size();
            recorded.clear();
            t = track;
        }
        printf("[texprewarm] track %d: race start recorded, %zu textures kept (%zu too new to check)\n", t, records.size(),
               unchecked);
        if (!records.empty() && t >= 0) save(t, std::move(records));
    }
}

namespace {
    // Hands the track's list to RT64 on a thread of its own. The track handed last, with no race since, is in RT64's
    // cache already (its textures stay until drawn): picked again (back to the track select and the same track), it is
    // left as it is, not unpinned and uploaded again.
    void prewarm(int menu_id) {
        if (prewarmed == menu_id && !raced) return;
        prewarmed = menu_id;
        raced = false;
        RT64::ActiveTextureUploadObserver = &recorder;
        std::thread([menu_id] {
            // Never two at once: wait out a list still being written (it may be this track's).
            while (busy.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            RT64::unpinPrewarmedTextures();   // the last track's that were never drawn age out
            std::vector<Record> list = read_list(menu_id);
            {
                std::lock_guard lock{ mutex };
                if (track == menu_id) {
                    for (const Record& r : list) known.insert(r.hash);
                }
            }
            for (Record& r : list) {
                RT64::prewarmTexture(r.hash, std::move(r.tmem), r.width, r.height, r.tlut, r.tile, r.decode != 0);
            }
            if (!list.empty()) printf("[texprewarm] track %d: prewarming %zu textures\n", menu_id, list.size());
            busy = false;
        }).detach();
    }

    // The attract demo's next track (race_next_attract_track_800AB824): the last one + 1, wrapping at 7, with ATARI
    // (10) after the 7th once it is unlocked.
    int next_attract_track(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id) + 1;
        if (t < 7) return t;
        return t == 7 && MEM_BU(0, (int32_t)unlock_tracks) != 0 ? 10 : 0;
    }
}

void rush2::texture_prewarm::set_track(int menu_id) {
    {
        std::lock_guard lock{ mutex };
        track = menu_id;
        recording = false;
        known.clear();
        recorded.clear();
        kept.clear();
    }
    picked = true;
    prewarm(menu_id);
}

void rush2::texture_prewarm::frame(uint8_t* rdram) {
    enum class Phase { menus, recording, raced };
    static Phase phase = Phase::menus;
    static int car_select_track = -1;
    static bool racing = false;
    static std::chrono::steady_clock::time_point racing_since;
    int32_t state = MEM_W(0, (int32_t)game_state);
    frame_count++;

    // Each visit to the car select, and each track picked there, prewarms that track.
    int at_car_select = state == 2 ? (int8_t)MEM_B(0, (int32_t)track_id) : -1;
    if (at_car_select != car_select_track) {
        car_select_track = at_car_select;
        if (at_car_select >= 0) set_track(at_car_select);
    }

    bool race = state == 7 || state == 10 || state == 9 || state == 3;
    if (!race) {
        if (phase == Phase::recording) finish();
        phase = Phase::menus;
        // The title screen (menu_title_screen_run_800ABE7C) starts the attract demo on its next track: that one is
        // prewarmed while the menus are up.
        if (state == 1) picked = false;
        if (state == 0 && !picked) {
            int next = next_attract_track(rdram);
            if (next != prewarmed) prewarm(next);
        }
        return;
    }
    if (phase == Phase::menus) {
        std::lock_guard lock{ mutex };
        // The attract demo races the stock track it set as the menu's (0x800ACFE0); a player's race is on the track
        // its car select had.
        if (MEM_BU(0, (int32_t)race_game_mode) != 0) {
            int demo_track = (int8_t)MEM_B(0, (int32_t)track_id);
            if (demo_track != track) known.clear();
            track = demo_track;
        }
        recorded.clear();
        kept.clear();
        recording = track >= 0;
        racing = false;
        picked = false;
        raced = true;
        phase = Phase::recording;
    }
    if (phase == Phase::recording) {
        sift();
        if (MEM_BU(0, (int32_t)race_game_mode) != 0) {
            // In case the demo flag is set after the race start state.
            std::lock_guard lock{ mutex };
            track = (int8_t)MEM_B(0, (int32_t)track_id);
        }
        if (state == 3 && !racing) {
            racing = true;
            racing_since = std::chrono::steady_clock::now();
        }
        if (racing && std::chrono::steady_clock::now() - racing_since > std::chrono::duration<double>(record_after_racing)) {
            finish();
            phase = Phase::raced;
        }
    }
}
