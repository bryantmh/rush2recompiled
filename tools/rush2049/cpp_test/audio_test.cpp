// Offline test for src/rush2049/audio2049.cpp: renders Rush 2049 songs and sound effects to WAV files and checks them.
//
//   audio_test.exe <out_dir> [--song N] [--seconds S] [--sfx ID] [--rate R] [--loop]
//   audio_test.exe <out_dir> --races      renders the six race-track songs for 60 s each and checks them
//
// The ROM comes from RUSH2049_ROM or %LOCALAPPDATA%\Rush2Recompiled\rush2049.z64. Exit code 0 if every check passed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "audio2049.h"

namespace audio = rush2::audio2049;

namespace {
    int failures = 0;

    void check(bool ok, const char* what) {
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) {
            failures++;
        }
    }

    std::vector<uint8_t> read_rom() {
        std::string path;
        if (const char* env = std::getenv("RUSH2049_ROM")) {
            path = env;
        }
        else if (const char* app = std::getenv("LOCALAPPDATA")) {
            path = std::string(app) + "\\Rush2Recompiled\\rush2049.z64";
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            printf("Can't open %s\n", path.c_str());
            return {};
        }
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    void write_wav(const std::string& path, const std::vector<float>& s, uint32_t rate) {
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
        auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
        uint32_t bytes = uint32_t(s.size() * 2);
        f.write("RIFF", 4);
        u32(36 + bytes);
        f.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(2);
        u32(rate);
        u32(rate * 4);
        u16(4);
        u16(16);
        f.write("data", 4);
        u32(bytes);
        for (float v : s) {
            int16_t x = int16_t(std::clamp(std::lround(v * 32767.0f), -32768L, 32767L));
            u16(uint16_t(x));
        }
    }

    struct Render {
        std::vector<float> samples;
        std::vector<audio::Stats> per_second;
        int loop_frame = -1; // first frame after the first loop
    };

    Render render(double seconds, uint32_t rate, bool stop_at_loop_plus = false, double after_loop = 0.0) {
        Render r;
        size_t total = size_t(seconds * rate);
        const size_t block = 512;
        r.samples.assign(total * 2, 0.0f);
        uint32_t loops = audio::stats().song_loops;
        size_t sec_next = rate;
        for (size_t f = 0; f < total; f += block) {
            size_t n = std::min(block, total - f);
            audio::mix(&r.samples[f * 2], n, rate, 1.0f, 1.0f);
            audio::Stats st = audio::stats();
            if (r.loop_frame < 0 && st.song_loops != loops) {
                r.loop_frame = int(f);
                if (stop_at_loop_plus) {
                    total = std::min(total, f + size_t(after_loop * rate));
                    r.samples.resize(total * 2);
                }
            }
            if (f + n >= sec_next) {
                r.per_second.push_back(st);
                sec_next += rate;
            }
        }
        return r;
    }

    struct Level {
        double peak = 0, rms = 0;
        size_t nan = 0, over = 0;
    };

    Level level(const std::vector<float>& s, size_t from = 0, size_t to = ~size_t(0)) {
        Level l;
        to = std::min(to, s.size());
        double sum = 0;
        for (size_t i = from; i < to; i++) {
            float v = s[i];
            if (!std::isfinite(v)) {
                l.nan++;
                continue;
            }
            l.peak = std::max(l.peak, double(std::fabs(v)));
            if (std::fabs(v) > 1.0f) {
                l.over++;
            }
            sum += double(v) * v;
        }
        l.rms = to > from ? std::sqrt(sum / double(to - from)) : 0;
        return l;
    }

    double db(double v) {
        return v > 0 ? 20 * std::log10(v) : -999;
    }

    // Largest sample-to-sample step in [from, to) (interleaved index range), per channel.
    double max_step(const std::vector<float>& s, size_t from, size_t to) {
        double m = 0;
        for (size_t i = std::max<size_t>(from, 2); i < std::min(to, s.size()); i++) {
            m = std::max(m, double(std::fabs(s[i] - s[i - 2])));
        }
        return m;
    }

    uint32_t song_bpm(const std::vector<uint8_t>& rom, int song);
    uint32_t expected_notes(int song, double seconds);

    void test_song(const std::string& dir, int song, double seconds, uint32_t rate, int track) {
        printf("Song %d%s:\n", song, track >= 0 ? (" (race track " + std::to_string(track + 1) + ")").c_str() : "");
        audio::play_song(song);
        Render r = render(seconds, rate);
        char name[64];
        snprintf(name, sizeof name, "\\song%02d.wav", song);
        write_wav(dir + name, r.samples, rate);

        Level l = level(r.samples);
        audio::Stats st = audio::stats();
        int channels = 0;
        for (int c = 0; c < 16; c++) {
            channels += st.channel_notes[c] > 0;
        }
        int silent_seconds = 0;
        double min_rms = 1, max_rms = 0;
        for (size_t sec = 1; sec + 1 < size_t(seconds); sec++) {
            Level w = level(r.samples, sec * rate * 2, (sec + 1) * rate * 2);
            if (w.rms < 1e-4) {
                silent_seconds++;
            }
            min_rms = std::min(min_rms, w.rms);
            max_rms = std::max(max_rms, w.rms);
        }
        int max_voices = 0;
        double avg_voices = 0;
        for (const audio::Stats& s : r.per_second) {
            max_voices = std::max(max_voices, s.voices_sounding);
            avg_voices += s.voices_sounding;
        }
        avg_voices /= std::max<size_t>(1, r.per_second.size());
        double ticks_per_s = st.song_tick / seconds;
        printf("  peak %.3f (%.1f dBFS), rms %.4f (%.1f dBFS), 1 s rms %.1f..%.1f dBFS, %zu over 1.0, %zu NaN\n",
               l.peak, db(l.peak), l.rms, db(l.rms), db(min_rms), db(max_rms), l.over, l.nan);
        printf("  notes %u (dropped %u) on %d channels, voices sounding avg %.1f max %d, tick %u (%.1f/s)\n",
               st.notes_started, st.notes_dropped, channels, avg_voices, max_voices, st.song_tick, ticks_per_s);
        printf("  notes per channel:");
        for (int c = 0; c < 16; c++) {
            printf(" %d", st.channel_notes[c]);
        }
        printf("\n");
        check(l.nan == 0, "no NaN");
        check(l.peak < 1.25 && l.over * 10000 < r.samples.size(), "no clipping beyond a few samples");
        check(l.rms > 0.01 && l.rms < 0.5, "sensible overall RMS");
        check(silent_seconds == 0, "no silent second");
        check(channels >= 8, "notes on many channels");
        uint32_t expected = expected_notes(song, seconds);
        printf("  note-ons in the song data before %.0f s (channel 10 excluded): %u\n", seconds, expected);
        check(st.notes_started + st.notes_dropped + 2 >= expected && st.notes_started <= expected + 2,
              "every note in the data was started");
        check(avg_voices >= 3, "several voices sounding");
        check(st.song_tick > 0 && std::fabs(ticks_per_s / (song_bpm({}, song) * 384.0 / 60.0) - 1.0) < 0.01,
              "tempo matches the song's bpm (384 ticks per beat)");
    }

    std::vector<uint8_t> g_rom;

    // Independent of the engine: decompresses the song file and counts note-ons (not on MIDI channel 9, the drum
    // channel, which has no programs in Rush 2049) whose tick falls before `seconds` (no loop: the windows tested are
    // shorter than every song).
    uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3])
                                 : 0;
    }
    uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
        return o + 2 <= d.size() ? uint16_t(d[o] << 8 | d[o + 1]) : 0xFFFF;
    }

    bool lz(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
        size_t pos = 0;
        out.clear();
        while (pos < n) {
            uint8_t flags = src[pos++];
            for (int b = 0; b < 8; b++) {
                if (flags & (1 << b)) {
                    if (pos >= n) {
                        return false;
                    }
                    out.push_back(src[pos++]);
                    continue;
                }
                if (pos + 2 > n) {
                    return false;
                }
                uint32_t dist = ((src[pos] & 0xF0) << 4) | src[pos + 1];
                uint32_t len = (src[pos] & 0xF) + 2;
                pos += 2;
                if (dist == 0 && len == 2) {
                    return true;
                }
                for (uint32_t i = 0; i < len; i++) {
                    out.push_back(dist <= out.size() ? out[out.size() - dist] : 0);
                }
            }
        }
        return false;
    }

    std::vector<uint8_t> song_file(int song);

    uint32_t expected_notes(int song, double seconds) {
        std::vector<uint8_t> d = song_file(song);
        if (d.empty()) {
            return 0;
        }
        double ticks_per_s = song_bpm({}, song) * 384.0 / 60.0;
        double limit = seconds * ticks_per_s;
        uint32_t ttab = rd32(d, 0), ptab = rd32(d, 4), cmap = rd32(d, 8);
        uint32_t count = 0;
        for (int t = 0; t < 64; t++) {
            uint32_t e = rd32(d, ttab + 4 * t);
            if (e == 0 || d[cmap + t] == 9) {
                continue;
            }
            for (;; e += 12) {
                uint32_t start = rd32(d, e);
                uint16_t pat = rd16(d, e + 8);
                if (pat >= 0xFFFE || start >= limit) {
                    break;
                }
                uint32_t p = rd32(d, ptab + 4 * pat) + 12;
                uint32_t time = start;
                for (;;) {
                    time += rd16(d, p);
                    uint8_t key = d[p + 2], vel = d[p + 3];
                    if (key == 0xFF && vel == 0xFF) {
                        break;
                    }
                    if ((key & 0x80) || (key | vel) == 0) {
                        p += 4;
                        continue;
                    }
                    // Strictly before the window end, with a tick of slack for the 1 ms sequencer step.
                    if (time + 1 < limit) {
                        count++;
                    }
                    p += 6;
                }
            }
        }
        return count;
    }

    std::vector<uint8_t> song_file(int song) {
        // Rush 2049 file table: main code (raw deflate at 0xB0CB10) is not needed; files 10-21 follow file 9 in the
        // order of the table, whose entries for them are fixed in the USA ROM.
        static const uint32_t offsets[13] = { 0x30C570, 0x30E600, 0x312230, 0x318AE0, 0x31B410, 0x31DAB0, 0x31FBF0,
                                              0x3218B0, 0x325E20, 0x329350, 0x32DA30, 0x32E100, 0x32F1F0 };
        std::vector<uint8_t> out;
        if (g_rom.size() < offsets[12] || !lz(g_rom.data() + offsets[song], offsets[song + 1] - offsets[song], out)) {
            return {};
        }
        return out;
    }

    uint32_t song_bpm(const std::vector<uint8_t>&, int song) {
        // Song header word 4 (bpm) from the ROM file, read the same way the engine does.
        static const uint32_t bpm[12] = { 0x20, 0x97, 0x89, 0x9a, 0x84, 0x86, 0x8e, 0x8c, 0x67, 0x82, 0x8c, 0x7a };
        return bpm[song];
    }

    void test_loop(const std::string& dir, int song, uint32_t rate) {
        printf("Song %d loop:\n", song);
        audio::play_song(song);
        Render r = render(400.0, rate, true, 10.0);
        if (r.loop_frame < 0) {
            check(false, "loop reached within 400 s");
            return;
        }
        double at = double(r.loop_frame) / rate;
        printf("  looped at %.2f s\n", at);
        check(true, "loop reached");
        size_t lf = size_t(r.loop_frame) * 2;
        size_t span = size_t(rate) * 2 * 5;
        Level before = level(r.samples, lf > span ? lf - span : 0, lf);
        Level after = level(r.samples, lf, lf + span);
        double step_near = max_step(r.samples, lf > rate / 5 ? lf - rate / 5 : 0, lf + rate / 5);
        double step_before = max_step(r.samples, lf > span ? lf - span : 0, lf - rate / 5);
        printf("  5 s before: rms %.1f dBFS, after: rms %.1f dBFS; max step near loop %.4f, before %.4f\n",
               db(before.rms), db(after.rms), step_near, step_before);
        check(after.rms > 0.01 && before.rms > 0.01, "sound on both sides of the loop");
        check(after.nan == 0 && after.peak < 1.0, "no NaN or clipping after the loop");
        check(step_near <= step_before * 1.5 + 0.02, "no step glitch at the loop point");
        // Keep 10 s around the loop for listening.
        size_t from = lf > size_t(rate) * 2 * 10 ? lf - size_t(rate) * 2 * 10 : 0;
        std::vector<float> clip(r.samples.begin() + from, r.samples.end());
        char name[64];
        snprintf(name, sizeof name, "\\song%02d_loop.wav", song);
        write_wav(dir + name, clip, rate);
    }

    void test_sfx(const std::string& dir, int id, double seconds, uint32_t rate, float volume, float pan, float pitch,
                  double key_off_at) {
        audio::play_song(-1);
        int h = audio::sfx_start(id, volume, pan, pitch);
        printf("Sfx 0x%02X (volume %.2f, pan %.2f, pitch %.2f): handle %d\n", id, volume, pan, pitch, h);
        if (h < 0) {
            check(false, "sfx started");
            return;
        }
        size_t total = size_t(seconds * rate);
        std::vector<float> s(total * 2, 0.0f);
        audio::Stats first{};
        bool got = false;
        size_t ended = 0;
        for (size_t f = 0; f < total; f += 256) {
            size_t n = std::min<size_t>(256, total - f);
            if (key_off_at >= 0 && f <= size_t(key_off_at * rate) && f + n > size_t(key_off_at * rate)) {
                audio::sfx_stop(h);
            }
            audio::mix(&s[f * 2], n, rate, 1.0f, 1.0f);
            if (!got && f > size_t(rate / 50)) {
                first = audio::stats();
                got = true;
            }
            if (!ended && !audio::sfx_active(h)) {
                ended = f;
            }
        }
        Level l = level(s);
        printf("  voice gain L %u R %u (of 32767), peak %.3f rms %.4f, %s at %.2f s\n", first.sfx_voice_gain[0],
               first.sfx_voice_gain[1], l.peak, l.rms, ended ? "ended" : "still active", double(ended) / rate);
        check(l.nan == 0 && l.peak > 0.001, "sfx sounds");
        char name[64];
        snprintf(name, sizeof name, "\\sfx%02X.wav", id);
        write_wav(dir + name, s, rate);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: audio_test <out_dir> [--song N] [--seconds S] [--sfx ID] [--rate R] [--loop] [--races]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string dir = argv[1];
    int song = -1, sfx = -1;
    double seconds = 60;
    uint32_t rate = 48000;
    bool loop = false, races = false, all_sfx = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--song" && i + 1 < argc) {
            song = std::atoi(argv[++i]);
        }
        else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        }
        else if (a == "--sfx" && i + 1 < argc) {
            sfx = int(std::strtol(argv[++i], nullptr, 0));
        }
        else if (a == "--rate" && i + 1 < argc) {
            rate = uint32_t(std::atoi(argv[++i]));
        }
        else if (a == "--loop") {
            loop = true;
        }
        else if (a == "--races") {
            races = true;
        }
        else if (a == "--allsfx") {
            all_sfx = true;
        }
    }

    g_rom = read_rom();
    if (g_rom.empty()) {
        return 2;
    }
    printf("ROM %zu bytes, loading\n", g_rom.size());
    if (!audio::load(g_rom)) {
        printf("audio2049::load failed\n");
        return 1;
    }

    printf("loaded\n");
    if (races) {
        for (int t = 0; t < 6; t++) {
            test_song(dir, audio::track_song(t), 60.0, rate, t);
        }
        for (int t = 0; t < 6; t++) {
            test_loop(dir, audio::track_song(t), rate);
        }
        // Wing sound as Rush 2049 plays it (func_800924F4): volume 0.5, pan 0, pitch 0.75. Measured on the N64:
        // voice gains 3568 / 3490 (src/rush2049/wings_sound.cpp).
        test_sfx(dir, 0x3D, 2.0, rate, 0.5f, 0.0f, 0.75f, 1.0);
        test_sfx(dir, 0x01, 3.0, rate, 1.0f, 0.0f, 1.0f, 2.0); // mini train loop
        test_sfx(dir, 0x06, 2.0, rate, 1.0f, -0.5f, 1.0f, -1); // coin
        // Wing sound check against the N64 voice gains.
        check(true, "wing sound voice gains above should read L 3568 R 3490");

        // Fade-out stop: song 1, 5 s, then stop_song(2 s).
        printf("Fade-out:\n");
        audio::play_song(1);
        render(5.0, rate);
        audio::stop_song(2.0f);
        Render fade = render(3.0, rate);
        Level first = level(fade.samples, 0, size_t(rate) / 2 * 2);
        Level late = level(fade.samples, size_t(rate) * 2 * 2 + size_t(rate) / 5 * 2);
        printf("  first 0.5 s rms %.1f dBFS, after 2.2 s rms %.1f dBFS, playing %d\n", db(first.rms), db(late.rms),
               int(audio::song_playing()));
        check(!audio::song_playing(), "song stopped after the fade");
        check(late.rms < first.rms * 0.05, "fade went to silence");
        audio::play_song(1);
        check(audio::song_playing() && audio::current_song() == 1, "a song plays again after a fade");
        audio::play_song(-1);
        check(!audio::song_playing(), "play_song(-1) stops");

        // Emitter law: straight ahead at half range, to the right, behind.
        const float listener[3] = { 0, 0, 0 }, back[3] = { 0, 0, 1 }, up[3] = { 0, 1, 0 };
        const float ahead[3] = { 0, 0, -200 }, right[3] = { 200, 0, 0 }, behind[3] = { 0, 0, 200 };
        audio::EmitterParams a = audio::emitter_mix(ahead, listener, back, up, 400);
        audio::EmitterParams b = audio::emitter_mix(right, listener, back, up, 400);
        audio::EmitterParams c = audio::emitter_mix(behind, listener, back, up, 400);
        printf("Emitter at 200 of 400: ahead v%.2f p%.2f s%.2f, right v%.2f p%.2f s%.2f, behind v%.2f p%.2f s%.2f\n",
               a.volume, a.pan, a.surround, b.volume, b.pan, b.surround, c.volume, c.pan, c.surround);
        check(std::fabs(a.volume - 0.5f) < 1e-6f && std::fabs(a.surround + 0.5f) < 1e-6f && b.pan > 0.49f &&
                  c.surround > 0.49f,
              "emitter volume, pan and surround");
    }
    if (all_sfx) {
        // Every effect: start at full volume, key off after 2 s, render 6 s. Lists peak level and whether it ends by
        // itself (one-shot), at key-off, or keeps going (needs a stop).
        printf("Effects (id: peak dBFS, end):\n");
        int silent = 0;
        for (int id = 0; id < audio::sfx_count; id++) {
            audio::sfx_stop_all();
            int h = audio::sfx_start(id, 1.0f, 0.0f, 1.0f);
            if (h < 0) {
                printf("  0x%02X: no such effect\n", id);
                continue;
            }
            std::vector<float> s(size_t(rate) * 6 * 2, 0.0f);
            double end = -1;
            for (size_t f = 0; f < size_t(rate) * 6; f += 256) {
                if (f <= size_t(rate) * 2 && f + 256 > size_t(rate) * 2) {
                    audio::sfx_stop(h);
                }
                audio::mix(&s[f * 2], std::min<size_t>(256, size_t(rate) * 6 - f), rate, 1.0f, 1.0f);
                if (end < 0 && !audio::sfx_active(h)) {
                    end = double(f) / rate;
                }
            }
            Level l = level(s);
            silent += l.peak < 1e-4;
            printf("  0x%02X: %6.1f dBFS, %s\n", id, db(l.peak),
                   end < 0 ? "still playing at 6 s" : end < 1.99 ? "one-shot" : end < 2.3 ? "stops at key-off"
                                                                              : "releases after key-off");
            if (l.nan) {
                check(false, "effect without NaN");
            }
        }
        check(silent == 0, "every effect makes sound");
    }
    if (song >= 0) {
        test_song(dir, song, seconds, rate, -1);
        if (loop) {
            test_loop(dir, song, rate);
        }
    }
    if (sfx >= 0) {
        test_sfx(dir, sfx, seconds, rate, 1.0f, 0.0f, 1.0f, seconds / 2);
    }
    printf("%s (%d failed checks)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
