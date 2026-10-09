// Rush 2049's ghost recording and playback as pure logic (see include/ghost_logic.h for the addresses).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ghost_logic.h"

namespace {
    constexpr char magic[8] = { 'R', '2', 'G', 'H', 'O', 'S', 'T', '3' };

    // The time step of the machine playing (0x8002AFB8): the recomp is always NTSC.
    constexpr float current_step = rush2::ghost::tick_step;

    // Rush 2049's race tracks are track select ids 12-17 (src/rush2049/track2049_menu.cpp).
    constexpr int first_2049_track = 12;
    constexpr float seconds_per_lap = 150.0f;
}

uint32_t rush2::ghost::capacity(int track, int laps) {
    // func_800F6AB8: (s32)((max_time[track] + 10.0) * 60.0).
    float max_time = seconds_per_lap * std::max(laps, 1);
    int k = track - first_2049_track;
    if (k >= 0 && k < 6) {
        max_time = std::max(max_time, max_time_2049[k]);
    }
    return (uint32_t)(int32_t)((float)((double)max_time + 10.0) * tick_rate);
}

void rush2::ghost::begin_recording(Ghost& g, const Header& header, uint32_t capacity) {
    // func_800F6AB8: +5 = -1, the car and player fields, +0x34 = the time step, +0x54 = the capacity, +0x4C = a new
    // buffer, +0x50 = 0.
    g = Ghost{};
    g.header = header;
    g.header.state = -1;
    g.header.step = current_step;
    g.header.count = capacity;
    g.header.position = 0;
    g.inputs.reserve(capacity);
    g.patch_start.reserve(capacity + 1);
    g.patch_start.push_back(0);
}

bool rush2::ghost::record(Ghost& g, const Input& in, const std::vector<Patch>& changed) {
    // func_800E5D64, recording branch: `if (position >= count) return 0;` then the sample at +0x4C[position] and
    // position + 1.
    if (g.header.state >= 0 || g.header.position >= g.header.count) {
        return false;
    }
    g.inputs.push_back(in);
    g.patches.insert(g.patches.end(), changed.begin(), changed.end());
    g.patch_start.push_back((uint32_t)g.patches.size());
    g.header.position++;
    return true;
}

int rush2::ghost::play(Ghost& g, int32_t tick_counter, Input& in, const Patch*& patches, size_t& patch_count,
                       const std::vector<uint32_t>*& keyframe) {
    patches = nullptr;
    patch_count = 0;
    keyframe = nullptr;
    if (g.header.state <= 0) {
        return 0;
    }
    // A PAL ghost on an NTSC machine skips every 6th tick (0x80143FF4 = the tick counter).
    if (current_step != g.header.step && g.header.step == pal_step) {
        if (std::abs(tick_counter) % 6 == 2) {
            return -1;
        }
    }
    uint32_t pos = g.header.position;
    if (g.header.count == pos || pos >= g.inputs.size()) {
        // Past the last sample: no buttons, no steering, throttle or brake (+0x732, +0x731, +0x720, +0x72C,
        // +0x728); the gear stays.
        in.wings = 0;
        in.steer = 0.0f;
        in.brake = 0.0f;
        in.throttle = 0.0f;
        in.stick_x = 0.0f;
        in.stick_y = 0.0f;
    }
    else {
        in = g.inputs[pos];
        if (pos + 1 < g.patch_start.size()) {
            patches = g.patches.data() + g.patch_start[pos];
            patch_count = g.patch_start[pos + 1] - g.patch_start[pos];
        }
        if (pos % keyframe_ticks == 0 && pos / keyframe_ticks < g.keyframes.size()) {
            keyframe = &g.keyframes[pos / keyframe_ticks];
        }
        g.header.position = pos + 1;
    }
    // An NTSC ghost on a PAL machine takes an extra step every 5th tick.
    if (current_step != g.header.step && g.header.step == ntsc_step) {
        if (std::abs(tick_counter) % 5 == 2) {
            return 1;
        }
    }
    return 0;
}

bool rush2::ghost::finish(Ghost& g, bool finished, float race_time) {
    // func_800D510C: a recording (+5 < 0) becomes idle (+5 = 0). If its car finished (car state +0xEF), +0x38 = the
    // race time and +0x54 = +0x50 (the sample count), and it is kept (0x80140800); otherwise its buffer is freed.
    if (g.header.state >= 0) {
        return false;
    }
    g.header.state = 0;
    if (!finished) {
        g = Ghost{};
        return false;
    }
    g.header.race_time = race_time;
    g.header.count = g.header.position;
    g.header.position = 0;
    return true;
}

bool rush2::ghost::better(const Ghost& candidate, const Ghost& best) {
    return best.inputs.empty() || candidate.header.race_time < best.header.race_time;
}

namespace {
    template <typename T>
    void put(std::ofstream& f, const T& v) {
        f.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }

    template <typename T>
    void put_vector(std::ofstream& f, const std::vector<T>& v) {
        put(f, (uint32_t)v.size());
        if (!v.empty()) {
            f.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
        }
    }

    template <typename T>
    bool get(std::ifstream& f, T& v) {
        return (bool)f.read(reinterpret_cast<char*>(&v), sizeof(T));
    }

    template <typename T>
    bool get_vector(std::ifstream& f, std::vector<T>& v, uint32_t max) {
        uint32_t n;
        if (!get(f, n) || n > max) {
            return false;
        }
        v.resize(n);
        return n == 0 || (bool)f.read(reinterpret_cast<char*>(v.data()), n * sizeof(T));
    }

    constexpr uint32_t max_items = 64u << 20;
}

// func_800CC50C moves the three sample streams (one byte each per sample) together behind each other, sized by the
// final count rather than the capacity, before compressing them for the pak. The file keeps the streams one after
// another the same way.
bool rush2::ghost::save(const std::filesystem::path& path, const Ghost& g) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(magic, sizeof(magic));
        Header h = g.header;
        h.state = 0;
        h.position = 0;
        h.count = (uint32_t)g.inputs.size();
        put(f, h);
        put(f, g.physics_time);
        put(f, g.game_time);
        put_vector(f, g.inputs);
        put_vector(f, g.patch_start);
        put_vector(f, g.patches);
        put_vector(f, g.start);
        put_vector(f, g.descriptor);
        put(f, (uint32_t)g.keyframes.size());
        for (const auto& k : g.keyframes) {
            put_vector(f, k);
        }
        if (!f) {
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

bool rush2::ghost::load(const std::filesystem::path& path, Ghost& g) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    char m[sizeof(magic)];
    if (!f.read(m, sizeof(m)) || std::memcmp(m, magic, sizeof(magic)) != 0) {
        return false;
    }
    Ghost r;
    uint32_t keyframes = 0;
    if (!get(f, r.header) || !get(f, r.physics_time) || !get(f, r.game_time) ||
        !get_vector(f, r.inputs, max_items) || !get_vector(f, r.patch_start, max_items) ||
        !get_vector(f, r.patches, max_items) || !get_vector(f, r.start, 0x1000) ||
        !get_vector(f, r.descriptor, 0x1000) || !get(f, keyframes) || keyframes > max_items) {
        return false;
    }
    r.keyframes.resize(keyframes);
    for (auto& k : r.keyframes) {
        if (!get_vector(f, k, 0x1000)) {
            return false;
        }
    }
    if (r.patch_start.size() != r.inputs.size() + 1 || r.patch_start.back() != r.patches.size() ||
        r.header.count != r.inputs.size()) {
        return false;
    }
    r.header.state = 0;
    r.header.position = 0;
    g = std::move(r);
    return true;
}

bool rush2::ghost::load_header(const std::filesystem::path& path, Header& h) {
    std::ifstream f(path, std::ios::binary);
    char m[sizeof(magic)];
    if (!f || !f.read(m, sizeof(m)) || std::memcmp(m, magic, sizeof(magic)) != 0 || !get(f, h)) {
        return false;
    }
    h.state = 0;
    h.position = 0;
    return true;
}
