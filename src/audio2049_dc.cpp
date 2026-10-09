// Rush 2049's sound from the Dreamcast disc. See docs/rush2049_research/dreamcast.md ("Audio").
//
// The disc has no MusyX: its sound effects are plain samples in .KAT banks and its songs are recorded streams.
// - Banks: table 0x8C0A7904, 9 x 0x24 bytes {name[0x14], u32 first id, u32 last id, u32 loaded, ...}. A sound's
//   global id (0x000-0x136) picks the bank whose range holds it (0x8C016174) and entry id - first in its .KAT. A
//   .KAT is u32 count, then count x 0x2C bytes {u32 1, u32 offset, u32 size (bytes), u32 rate, u32 loops, u32 format
//   (4 Yamaha/AICA ADPCM, 8 signed PCM8, 16 PCM16), u32, u32 loop start, u32 loop end (samples), u32, u32}.
// - Volume (0x8C016508): x -> 1 - (1 - x)^2 -> index x 255 into the attenuation table 0x8C0BADA8 (u8[256]), sent as
//   127 - a / 2; every sound but 0x99 (the plane) is scaled by 0.8 first (0x8C016210). The attenuation is taken as
//   AICA total-level steps of 0.375 dB [I]. Pan goes out as (1 - p) / 2 x 127 [the sound library's pan law isn't
//   traced; an equal-power pan stands in].
// - Songs: list 0x8C0BA818 of 20 file names (18 .STR, then HighScore.rom and Select.rom); per-track song table
//   0x8C0BA76C, s32 per 2049 track id 0-18 (-1 random). The pack keeps the .STR songs as IMA ADPCM
//   (src/rush2049_dc.cpp); the .ROM ones are raw mono PCM16, taken as 22050 Hz looping [I].
//
// The port asks for N64 effect ids and N64 songs. Effects go through the pairing tools/rush2049/dc_sounds.py made
// (rush2049_dc_sounds.inc). A song is the disc's song for the track the port says it's racing (set_track) when the N64
// plays that song there, else the disc's song for the first track the N64 plays it on; the N64's menu song 6 is
// Select.rom.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "audio2049_dc.h"
#include "rush2049_dc_internal.h"

namespace {
    using Source = rush2::rom2049::Source;

    constexpr uint32_t exe_base = 0x8C010000;
    constexpr uint32_t bank_table = 0x8C0A7904;
    constexpr int bank_count = 9;
    constexpr uint32_t song_files = 0x8C0BA818;     // char* x 20
    constexpr int song_file_count = 20;
    constexpr int song_select = 19;                 // Select.rom
    constexpr int song_highscore = 18;              // HighScore.rom
    constexpr uint32_t track_song_table = 0x8C0BA76C;
    constexpr int track_count = 19;
    constexpr uint32_t attenuation_table = 0x8C0BADA8;
    constexpr int plane_sound = 0x99;               // played without the 0.8 scale (0x8C016250)
    constexpr int voice_count = 32;
    constexpr float release_seconds = 0.01f;
    // audio2049's callers (src/track2049_audio.cpp) boost its MusyX output to make up for MusyX's headroom (music
    // 2.8x, effects 2x, times 127/114 for Rush 2049's default master volume). The disc's streams and samples are
    // already at full level, so they're scaled back to play at their own level at full volume.
    constexpr float musyx_full = 127.0f / 114.0f;
    constexpr float music_scale = 1.0f / (2.8f * musyx_full);
    constexpr float sfx_scale = 1.0f / (2.0f * musyx_full);

    struct SoundPair {
        int n64, dc;
    };
    const SoundPair sound_pairs[] = {
#define SOUND(n64, dc) { n64, dc },
#include "rush2049_dc_sounds.inc"
    };
    const int n64_track_songs[track_count] = {
#define N64_TRACK_SONG(track, song) song,
#include "rush2049_dc_sounds.inc"
    };

    struct Sample {
        std::vector<int16_t> pcm;
        uint32_t rate = 22050;
        uint32_t loop_start = 0, loop_end = 0; // loop_end 0: one-shot
    };

    struct Voice {
        const Sample* s = nullptr;
        uint32_t serial = 0;
        double pos = 0.0;
        float gain_l = 0.0f, gain_r = 0.0f;
        float pitch = 1.0f;
        bool releasing = false;
        float release = 1.0f;
    };

    struct Song {
        int file = -1;          // index into the disc's song list
        int n64 = -1;           // the N64 song it stands for
        std::vector<int16_t> pcm; // interleaved stereo
        uint32_t rate = 22050;
        size_t frames = 0, loop_frame = 0;
        double pos = 0.0;
        float fade = 1.0f, fade_step = 0.0f;
    };

    struct State {
        std::shared_ptr<const Source> source;
        std::array<Sample, 0x140> sounds;
        std::array<bool, 0x140> present{};
        std::array<std::string, song_file_count> song_names;
        std::array<int, track_count> dc_track_songs{};
        std::array<uint8_t, 256> attenuation{};
        std::array<Voice, voice_count> voices;
        uint32_t next_serial = 1;
        Song song;
        bool song_on = false;
        int track = -1;
        uint32_t song_request = 0;  // bumped by every play and stop; a song read in the background lands only if it's the latest
    };

    std::mutex mutex;
    std::unique_ptr<State> st;

    uint32_t le32(const std::vector<uint8_t>& d, size_t o) {
        return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    }

    bool exe_word(const std::vector<uint8_t>& exe, uint32_t a, uint32_t& v) {
        if (a < exe_base || a - exe_base + 4 > exe.size()) return false;
        v = le32(exe, a - exe_base);
        return true;
    }

    std::string exe_string(const std::vector<uint8_t>& exe, uint32_t a) {
        std::string s;
        for (size_t o = a - exe_base; a >= exe_base && o < exe.size() && exe[o] && s.size() < 64; o++) s += (char)exe[o];
        return s;
    }

    std::string upper(std::string s) {
        for (char& c : s) c = (char)toupper((unsigned char)c);
        return s;
    }

    // Yamaha AICA ADPCM: 4 bits a sample, low nibble first.
    void decode_aica(const uint8_t* in, size_t bytes, std::vector<int16_t>& out) {
        static const int diff[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
        static const int scale[8] = { 0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266 };
        int hist = 0, step = 0x7F;
        out.reserve(bytes * 2);
        for (size_t i = 0; i < bytes; i++) {
            for (int n : { in[i] & 0xF, in[i] >> 4 }) {
                hist = std::clamp(hist + step * diff[n] / 8, -32768, 32767);
                step = std::clamp((step * scale[n & 7]) >> 8, 0x7F, 0x6000);
                out.push_back((int16_t)hist);
            }
        }
    }

    bool load_bank(const Source& src, const std::string& file, int first, int last, State& s) {
        std::vector<uint8_t> d;
        if (!src.disc_file(upper(file), d) || d.size() < 4) return false;
        uint32_t count = le32(d, 0);
        for (uint32_t i = 0; i < count && first + (int)i <= last && first + (int)i < (int)s.sounds.size(); i++) {
            size_t r = 4 + (size_t)i * 0x2C;
            if (r + 0x2C > d.size()) return false;
            uint32_t offset = le32(d, r + 4), size = le32(d, r + 8), rate = le32(d, r + 12), loops = le32(d, r + 16);
            uint32_t format = le32(d, r + 20), loop_start = le32(d, r + 28), loop_end = le32(d, r + 32);
            if ((uint64_t)offset + size > d.size() || rate == 0) continue;
            Sample& smp = s.sounds[first + i];
            smp.rate = rate;
            const uint8_t* p = &d[offset];
            if (format == 4) {
                decode_aica(p, size, smp.pcm);
            }
            else if (format == 8) {
                for (uint32_t k = 0; k < size; k++) smp.pcm.push_back((int16_t)((int8_t)p[k] * 256));
            }
            else if (format == 16) {
                for (uint32_t k = 0; k + 1 < size; k += 2) smp.pcm.push_back((int16_t)(p[k] | (p[k + 1] << 8)));
            }
            else {
                continue;
            }
            if (loops && loop_end > loop_start) {
                smp.loop_start = std::min<uint32_t>(loop_start, (uint32_t)smp.pcm.size());
                smp.loop_end = std::min<uint32_t>(loop_end, (uint32_t)smp.pcm.size());
            }
            s.present[first + i] = !smp.pcm.empty();
        }
        return true;
    }

    int dc_sound(int n64) {
        if (n64 >= 0x1000) return n64 - 0x1000; // a disc id the N64 has no counterpart for
        for (const SoundPair& p : sound_pairs) {
            if (p.n64 == n64) return p.dc;
        }
        return -1;
    }

    // Volume (0-1) to a gain, the disc's way (0x8C016508).
    float volume_gain(const State& s, float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        float v = 1.0f - (1.0f - x) * (1.0f - x);
        int a = s.attenuation[(uint8_t)(int)(v * 255.0f)] & ~1;
        return a >= 254 ? 0.0f : std::pow(10.0f, -0.375f * (float)a / 20.0f);
    }

    void set_gains(const State& s, Voice& v, int id, float volume, float pan, float pitch) {
        float g = volume_gain(s, volume) * (id == plane_sound ? 1.0f : 0.8f);
        float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
        v.gain_l = g * std::min(1.0f, std::cos(angle) * 1.41421356f);
        v.gain_r = g * std::min(1.0f, std::sin(angle) * 1.41421356f);
        v.pitch = std::clamp(pitch, 0.0f, 4.0f);
    }

    Voice* voice_by_handle(State& s, int handle) {
        int slot = handle & 0xFF;
        if (handle < 0 || slot >= voice_count) return nullptr;
        Voice& v = s.voices[slot];
        return v.s != nullptr && v.serial == (uint32_t)handle >> 8 ? &v : nullptr;
    }

    float cubic(const std::vector<int16_t>& d, size_t stride, size_t channel, int64_t i, float t, size_t n) {
        auto at = [&](int64_t k) { return k < 0 || (size_t)k >= n ? 0.0f : (float)d[(size_t)k * stride + channel]; };
        float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    }

    // The disc song standing for song n: the disc's own (rush2::audio2049::disc_songs + n), or N64 song n on the
    // current track.
    int song_file(const State& s, int n) {
        if (n >= rush2::audio2049::disc_songs && n < rush2::audio2049::disc_songs + song_file_count) {
            return n - rush2::audio2049::disc_songs;
        }
        if (s.track >= 0 && s.track < track_count && n64_track_songs[s.track] == n && s.dc_track_songs[s.track] >= 0) {
            return s.dc_track_songs[s.track];
        }
        for (int t = 0; t < track_count; t++) {
            if (n64_track_songs[t] == n && s.dc_track_songs[t] >= 0) return s.dc_track_songs[t];
        }
        if (n == 6) return song_select;
        if (n == 5) return song_highscore;
        return -1;
    }

    // Reads and decodes a song (on a worker thread; a few hundred ms for a long one).
    bool read_song(const Source& src, const std::string& name, Song& out) {
        std::vector<uint8_t> d;
        if (!src.disc_file(upper(name), d)) return false;
        std::string ext = upper(name.substr(name.size() >= 4 ? name.size() - 4 : 0));
        if (ext == ".ROM") {
            out.rate = 22050;
            out.frames = d.size() / 2;
            out.pcm.resize(out.frames * 2);
            for (size_t i = 0; i < out.frames; i++) {
                int16_t v = (int16_t)(d[i * 2] | (d[i * 2 + 1] << 8));
                out.pcm[i * 2] = out.pcm[i * 2 + 1] = v;
            }
            out.loop_frame = 0;
            return out.frames > 0;
        }
        // .STR in the pack: the disc's 0x800-byte header (u32 1, rate, bits, block bytes, blocks, data bytes,
        // channels, end block, loop block), then per block and channel 4 + block_samples / 2 bytes of IMA ADPCM.
        constexpr size_t header = 0x800;
        constexpr int n = rush2::rom2049::dc::music_block_samples;
        constexpr size_t block = 4 + n / 2;
        if (d.size() < header) return false;
        uint32_t rate = le32(d, 4), channels = le32(d, 24), end_block = le32(d, 28), loop_block = le32(d, 32);
        if (channels != 2 || rate == 0) return false;
        size_t blocks = (d.size() - header) / (block * 2);
        if (end_block == 0 || end_block > blocks) end_block = (uint32_t)blocks;
        if (loop_block >= end_block) loop_block = 0;
        out.rate = rate;
        out.frames = (size_t)end_block * n;
        out.loop_frame = (size_t)loop_block * n;
        out.pcm.resize(out.frames * 2);
        std::vector<int16_t> ch(n);
        for (size_t b = 0; b < end_block; b++) {
            for (int c = 0; c < 2; c++) {
                rush2::rom2049::dc::adpcm_decode(&d[header + (b * 2 + c) * block], n, ch.data());
                for (int i = 0; i < n; i++) out.pcm[(b * n + i) * 2 + c] = ch[i];
            }
        }
        return true;
    }
}

bool rush2::audio2049::dc::load(std::shared_ptr<const Source> source) {
    auto s = std::make_unique<State>();
    s->source = source;
    std::vector<uint8_t> exe;
    if (source == nullptr || !source->disc_file("1ST_READ.BIN", exe)) return false;
    for (int t = 0; t < track_count; t++) {
        uint32_t v = 0;
        if (!exe_word(exe, track_song_table + t * 4, v)) return false;
        s->dc_track_songs[t] = (int32_t)v >= 0 && (int32_t)v < song_file_count ? (int)v : -1;
    }
    for (int i = 0; i < song_file_count; i++) {
        uint32_t p = 0;
        if (!exe_word(exe, song_files + i * 4, p)) return false;
        s->song_names[i] = exe_string(exe, p);
    }
    if (attenuation_table - exe_base + 256 > exe.size()) return false;
    memcpy(s->attenuation.data(), &exe[attenuation_table - exe_base], 256);
    int banks = 0;
    for (int b = 0; b < bank_count; b++) {
        uint32_t a = bank_table + b * 0x24, first = 0, last = 0;
        if (!exe_word(exe, a + 0x14, first) || !exe_word(exe, a + 0x18, last)) return false;
        // unused1.kat and unused2.kat aren't on the disc.
        banks += load_bank(*source, exe_string(exe, a), (int)first, (int)last, *s);
    }
    if (banks == 0) return false;
    std::lock_guard lock{ mutex };
    st = std::move(s);
    return true;
}

void rush2::audio2049::dc::unload() {
    std::lock_guard lock{ mutex };
    st.reset();
}

bool rush2::audio2049::dc::active() {
    std::lock_guard lock{ mutex };
    return st != nullptr;
}

void rush2::audio2049::dc::set_track(int track_id) {
    std::lock_guard lock{ mutex };
    if (st) st->track = track_id;
}

int rush2::audio2049::dc::track_song(int track_id) {
    std::lock_guard lock{ mutex };
    if (!st || track_id < 0 || track_id >= track_count || st->dc_track_songs[track_id] < 0) return -1;
    return rush2::audio2049::disc_songs + st->dc_track_songs[track_id];
}

void rush2::audio2049::dc::play_song(int song) {
    std::shared_ptr<const Source> source;
    std::string name;
    uint32_t request;
    {
        std::lock_guard lock{ mutex };
        if (!st) return;
        st->song_on = false;
        st->song = Song{};
        request = ++st->song_request;
        int file = song_file(*st, song);
        if (file < 0 || st->song_names[file].empty()) return;
        source = st->source;
        name = st->song_names[file];
        st->song.file = file;
        st->song.n64 = song;
        st->song_on = true; // playing (silently) while it loads, as far as the callers can tell
    }
    std::thread([source, name, song, request]() {
        auto loaded = std::make_unique<Song>();
        bool ok = read_song(*source, name, *loaded);
        std::lock_guard lock{ mutex };
        if (!st || st->song_request != request) return;
        if (!ok) {
            fprintf(stderr, "[2049] Couldn't read the disc's song %s\n", name.c_str());
            st->song_on = false;
            return;
        }
        loaded->file = st->song.file;
        loaded->n64 = song;
        loaded->fade = st->song.fade;
        loaded->fade_step = st->song.fade_step;
        st->song = std::move(*loaded);
    }).detach();
}

void rush2::audio2049::dc::stop_song(float fade_seconds) {
    std::lock_guard lock{ mutex };
    if (!st || !st->song_on) return;
    if (fade_seconds <= 0.0f) {
        st->song_on = false;
        st->song = Song{};
        st->song_request++;
        return;
    }
    st->song.fade_step = 1.0f / fade_seconds;
}

bool rush2::audio2049::dc::song_playing() {
    std::lock_guard lock{ mutex };
    return st && st->song_on;
}

int rush2::audio2049::dc::current_song() {
    std::lock_guard lock{ mutex };
    return st && st->song_on ? st->song.n64 : -1;
}

int rush2::audio2049::dc::sfx_start(int id, float volume, float pan, float pitch, float surround) {
    std::lock_guard lock{ mutex };
    int dc = dc_sound(id);
    if (!st || dc < 0 || dc >= (int)st->sounds.size() || !st->present[dc]) return -1;
    int slot = -1;
    for (int i = 0; i < voice_count && slot < 0; i++) {
        if (st->voices[i].s == nullptr) slot = i;
    }
    if (slot < 0) return -1;
    Voice& v = st->voices[slot];
    v = Voice{};
    v.s = &st->sounds[dc];
    v.serial = st->next_serial++ & 0x7FFFFF;
    if (v.serial == 0) v.serial = st->next_serial++ & 0x7FFFFF;
    set_gains(*st, v, dc, volume, pan, pitch);
    return (int)(v.serial << 8 | (uint32_t)slot);
}

void rush2::audio2049::dc::sfx_update(int handle, float volume, float pan, float pitch, float surround) {
    std::lock_guard lock{ mutex };
    if (!st) return;
    if (Voice* v = voice_by_handle(*st, handle)) {
        int dc = (int)(v->s - st->sounds.data());
        set_gains(*st, *v, dc, volume, pan, pitch);
    }
}

void rush2::audio2049::dc::sfx_stop(int handle) {
    std::lock_guard lock{ mutex };
    if (!st) return;
    if (Voice* v = voice_by_handle(*st, handle)) v->releasing = true;
}

bool rush2::audio2049::dc::sfx_active(int handle) {
    std::lock_guard lock{ mutex };
    return st && voice_by_handle(*st, handle) != nullptr;
}

void rush2::audio2049::dc::sfx_stop_all() {
    std::lock_guard lock{ mutex };
    if (!st) return;
    for (Voice& v : st->voices) v = Voice{};
}

bool rush2::audio2049::dc::sfx_samples(int id, std::vector<int16_t>& pcm, uint32_t& rate) {
    std::lock_guard lock{ mutex };
    int dc = dc_sound(id);
    if (!st || dc < 0 || dc >= (int)st->sounds.size() || !st->present[dc]) return false;
    const Sample& s = st->sounds[dc];
    size_t end = s.loop_end ? s.loop_end : s.pcm.size();
    pcm.assign(s.pcm.begin() + s.loop_start, s.pcm.begin() + end);
    rate = s.rate;
    return !pcm.empty();
}

void rush2::audio2049::dc::mix(float* out, size_t frames, uint32_t sample_rate, float music_gain, float sfx_gain) {
    std::lock_guard lock{ mutex };
    if (!st || sample_rate == 0) return;
    const float dt = 1.0f / (float)sample_rate;
    // Effects.
    float fx = sfx_gain * sfx_scale / 32768.0f;
    for (Voice& v : st->voices) {
        if (v.s == nullptr) continue;
        const Sample& s = *v.s;
        size_t n = s.pcm.size();
        double step = (double)s.rate * v.pitch / sample_rate;
        for (size_t k = 0; k < frames; k++) {
            if (v.releasing) {
                v.release -= dt / release_seconds;
                if (v.release <= 0.0f) {
                    v = Voice{};
                    break;
                }
            }
            int64_t i = (int64_t)v.pos;
            float x = cubic(s.pcm, 1, 0, i, (float)(v.pos - (double)i), n) * fx * v.release;
            out[k * 2] += x * v.gain_l;
            out[k * 2 + 1] += x * v.gain_r;
            v.pos += step;
            if (s.loop_end != 0) {
                while (v.pos >= s.loop_end) v.pos -= (double)(s.loop_end - s.loop_start);
            }
            else if (v.pos >= (double)n) {
                v = Voice{};
                break;
            }
        }
    }
    // The song.
    Song& song = st->song;
    if (st->song_on && song.frames > 0) {
        float g = music_gain * music_scale / 32768.0f;
        double step = (double)song.rate / sample_rate;
        for (size_t k = 0; k < frames; k++) {
            if (song.fade_step > 0.0f) {
                song.fade -= song.fade_step * dt;
                if (song.fade <= 0.0f) {
                    st->song_on = false;
                    st->song = Song{};
                    st->song_request++;
                    break;
                }
            }
            int64_t i = (int64_t)song.pos;
            float t = (float)(song.pos - (double)i);
            out[k * 2] += cubic(song.pcm, 2, 0, i, t, song.frames) * g * song.fade;
            out[k * 2 + 1] += cubic(song.pcm, 2, 1, i, t, song.frames) * g * song.fade;
            song.pos += step;
            if (song.pos >= (double)song.frames) song.pos -= (double)(song.frames - song.loop_frame);
        }
    }
}

void rush2::audio2049::dc::stats(Stats& out) {
    std::lock_guard lock{ mutex };
    if (!st) return;
    out.song = st->song_on ? st->song.n64 : -1;
    for (const Voice& v : st->voices) {
        if (v.s != nullptr) {
            out.voices_sfx++;
            out.voices_sounding++;
        }
    }
}
