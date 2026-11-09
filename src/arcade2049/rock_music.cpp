// San Francisco Rush: The Rock's music (include/rock_music.h; docs/sfrush_arcade.md#music has the full description).
//
// The arcade's sound board (Atari CAGE style: a TMS32C031 on a 32-bit ROM bus) plays sequenced songs: a script per
// track drives a voice that plays the ROM's samples. ProjectR (project-r.exe) carries a native port of that program,
// which this follows:
//   - ROM (loader 0x1401B7E80): dword k of the 8 MB image is u62[2k], u62[2k+1], u61[2k], u61[2k+1] (little endian)
//     for the first 4 MB, u53/u49 the same for the second. DSP address = 0xD00000 + dword index.
//   - Floats are TMS320C3x (0x140243320): top byte the signed exponent, bit 23 the sign, 23-bit mantissa; exponent
//     -128 is 0.
//   - Sample records (parse 0x1401B8B80): 256 pointers at byte 0x7E5800, each to dwords R[0..16]: R[1] format (3 8-bit,
//     1 16-bit, 5 unsupported), R[2] rate (float), R[3] start, R[4] length, R[7]/R[8] loop start/end (a loop when
//     start < end), R[10] root key, R[13] attack and R[16] release step per block (floats, negative: none).
//     8-bit samples: four per dword, most significant byte first, x 256 (0x1402DD650); 16-bit: two per dword, high
//     half first (0x1402DD3E0).
//   - Tables: 0xEFC963 semitone ratios (73), 0xEFC9AB cent ratios (457), 0xEFCB74 volume curve (256), 0xEFC8E3
//     velocity curve (128), 0xEFCDB4 the pan lobe (130).
//   - Sequencer (tick 0x14024EDB0, spawn 0x14024E810): 250 ticks a second; each tick every track's counter runs
//     down by its tempo and its script runs to the next note when it passes 0. Two dwords an instruction (opcode,
//     operand; branches a third); opcodes below 0x80 are notes (0 a rest), the rest the table below. ProjectR ticks
//     once per 64-sample block (344.5 a second), which plays every song 1.378x too fast: the songs' phrase samples
//     (22050 Hz, in tune at that rate) last exactly a whole note at 250 ticks (88.2 samples a tick), e.g. Sunset's
//     3.2 s drum loop is two whole notes of 400 ticks.
//   - Voices (0x14024B2D0, render 0x1402DDBA0): linear interpolation, rate = semitone x cents x the sample's rate,
//     level = velocity x volume x envelope, panned by the lobe as ProjectR's stereo mix does (0x14024AA20).
// Effects the songs barely use (vibrato, the 0xA4 / 0x97 modulation, reverb sends, portamento) aren't ported.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "rock_music.h"

namespace {
    constexpr uint32_t dsp_base = 0xD00000;
    constexpr size_t rom_dwords = 0x200000;     // 8 MB
    constexpr int block = 64;
    constexpr uint32_t sample_rate = 22050;
    constexpr uint32_t tick_rate = 250;     // sequencer ticks a second (see the top)
    constexpr int64_t forever = 0x7FFFFFFF;

    struct Record {
        int format = 0;
        float rate = 1.0f;
        uint32_t loop_start = 0, loop_end = 0;
        bool loops = false;
        int root = 60;
        float attack = -1.0f, release = -1.0f;
        std::vector<float> pcm;     // 16-bit scale
    };

    struct Rom {
        std::vector<uint32_t> w;
        std::array<float, 73> semitones{};
        std::array<float, 457> cents{};
        std::array<float, 256> volume{};
        std::array<float, 128> velocity{};
        std::array<float, 130> lobe{};
        std::array<Record, 256> records;

        uint32_t at(uint32_t a) const {
            uint32_t i = a - dsp_base;
            return i < w.size() ? w[i] : 0;
        }
    };

    std::mutex rom_mutex;
    std::shared_ptr<const Rom> rom_data;

    float c3x(uint32_t v) {
        int e = (int8_t)(v >> 24);
        if (e == -128) return 0.0f;
        float f = (float)(v & 0x7FFFFF) / (float)(1 << 23);
        return std::ldexp((v >> 23) & 1 ? f - 2.0f : 1.0f + f, e);
    }

    template <size_t N>
    void table(const Rom& r, uint32_t addr, std::array<float, N>& out) {
        for (size_t i = 0; i < N; i++) out[i] = c3x(r.at(addr + (uint32_t)i));
    }

    // The song table's music entries (project-r.exe 0x1406EB6C0, 16 bytes each: the track list, type 8 = start song):
    // each track's script start (DSP address), priority and channel. Taken from the exe once; the scripts are the ROM's.
    struct TrackStart {
        uint32_t pc;
        int priority, channel;
    };
    struct SongDef {
        int command;
        std::vector<TrackStart> tracks;
    };
    // The race tracks' songs, by TRACKn (n - 1): the game's per-track starter (project-r.exe 0x1402303A0, its table
    // 0x14065B626: sound calls 0x8052, 0x8050, 0x8056, 0x80D8, 0x8054, 0x8058, 0x805A) through the call table
    // 0x1406E4200 to these commands. TRACK4 (The Rock) has the one call out of the run. Command 29 (TRACK5) plays the
    // same scores as command 3. The other music commands aren't race songs: 37 (23 s) and 61 (154 s) play once.
    const SongDef song_defs[] = {
        { 27, { { 0xEFA0E5, 195, 1 }, { 0xEFA12B, 120, 2 }, { 0xEFA1BD, 160, 3 } } },
        { 25, { { 0xEFB0C1, 195, 1 }, { 0xEFB0DD, 120, 2 }, { 0xEFB1AF, 160, 3 }, { 0xEFB357, 80, 4 } } },
        { 31, { { 0xEFA35D, 195, 1 }, { 0xEFA381, 40, 2 }, { 0xEFA41F, 80, 3 }, { 0xEFA4B3, 160, 4 }, { 0xEFA4F3, 120, 5 } } },
        { 41, { { 0xEFBC0D, 100, 1 }, { 0xEFBCF9, 180, 2 }, { 0xEFBD47, 80, 3 }, { 0xEFBF49, 160, 4 }, { 0xEFC079, 120, 5 } } },
        { 29, { { 0xEF9D3B, 195, 1 }, { 0xEF9DCB, 120, 2 }, { 0xEF9E63, 80, 3 }, { 0xEF9EFB, 160, 4 } } },
        { 33, { { 0xEFA59B, 195, 1 }, { 0xEFA64F, 160, 2 }, { 0xEFA99D, 120, 3 }, { 0xEFAAE7, 80, 4 }, { 0xEFAE05, 40, 5 } } },
        { 35, { { 0xEFB4B1, 195, 1 }, { 0xEFB4DD, 120, 2 }, { 0xEFB677, 160, 3 }, { 0xEFB8F9, 80, 4 } } },
    };

    constexpr int64_t durations[16] = { 0, 0x3C0000, 0x1E0000, 0xF0000, 0x78000, 0x140000, 0xA0000, 0x50000, 0xC0000,
                                        0x3C000, 0x1E000, 0xF000, 0x7800, 0x28000, 0x14000, 0x60000 };
    constexpr int accents[4] = { 0, 0, 20, 40 };

    struct Voice {
        const Record* r = nullptr;
        bool on = false, releasing = false, loops = false;
        double pos = 0.0;
        float level = 1.0f, attack = -1.0f;
        int key = 0, cents = 0, velocity = 0, volume = 0, angle = 0;

        void trigger(const Record& rec, int k, int c) {
            r = &rec;
            pos = 0.0;
            on = !rec.pcm.empty();
            releasing = false;
            level = 1.0f;
            loops = rec.loops;
            key = k;
            cents = c;
            attack = -1.0f;
            if (rec.attack >= 0.0f) {
                level = std::max(rec.attack, 1e-6f);
                attack = rec.attack;
            }
        }

        void release() {
            if (!on) return;
            releasing = true;
            if (r->release < 0.0f) loops = false;   // no release envelope: play out to the sample's end
        }

        double ratio(const Rom& rom) const {
            int q = cents / 100;
            int rem = cents - q * 100;
            int semis = std::clamp(key + q - r->root, -36, 36);
            return (double)rom.semitones[(size_t)(semis + 36)] * rom.cents[(size_t)std::clamp(rem + 100, 0, 456)] *
                   r->rate;
        }

        float lobe(const Rom& rom, int x) const {
            x &= 0x1FF;
            return x > 0x100 ? 0.0f : rom.lobe[(size_t)(x < 0x81 ? x : 0x100 - x)];
        }

        void render(const Rom& rom, float* out) {
            if (!on) return;
            double step = ratio(rom);
            const std::vector<float>& p = r->pcm;
            size_t n = p.size();
            double span = (double)r->loop_end - r->loop_start;
            float amp = rom.velocity[(size_t)std::clamp(velocity, 0, 127)] * rom.volume[(size_t)(volume & 0xFF)] * level;
            constexpr float side = 0.70710678f;
            float gl = (lobe(rom, angle + 0xC0) + side * lobe(rom, angle + 0x140)) * amp;
            float gr = (lobe(rom, angle + 0x40) + side * lobe(rom, angle + 0x1C0)) * amp;
            for (int k = 0; k < block; k++) {
                double x = pos + k * step;
                if (loops && x >= r->loop_end && span > 0) x = r->loop_start + std::fmod(x - r->loop_start, span);
                size_t i = (size_t)x;
                if (i + 1 >= n) continue;
                float s = p[i] + (p[i + 1] - p[i]) * (float)(x - (double)i);
                out[k * 2] += s * gl;
                out[k * 2 + 1] += s * gr;
            }
            pos += block * step;
            if (loops && span > 0) {
                while (pos > r->loop_end) pos -= span;
            }
            else if (pos >= (double)n) {
                on = false;
            }
            if (attack >= 0.0f && level < 1.0f && !releasing) level = std::min(1.0f, level + attack);
            if (releasing && r->release >= 0.0f) {
                level -= r->release;
                if (level <= 0.0f) on = false;
            }
        }
    };

    struct Track {
        uint32_t pc;
        Voice* v;
        std::array<int32_t, 32>* globals;
        bool alive = true;
        int64_t c20 = 0, c24 = forever;
        int32_t tempo = 0x1000, trans = 0, cents = 0, vol = 0, vel = 0, sample = -1, reg = 0, cmp = 0, pan = 0;
        uint32_t patch = 0;
        bool tie = false, sounding = false;
        std::vector<uint32_t> calls;
        std::vector<std::pair<uint32_t, int32_t>> loops;

        int32_t var(int a) const {
            if (a >= 6 && a <= 0x25) return (*globals)[(size_t)(a - 6)];
            switch (a) {
                case 0: return vol;
                case 1: return tempo;
                case 2: return trans;
                case 3: return (cents >> 3) & 0xFF;
                case 4: return cents;
                case 0xFE: return reg;
                case 0xFF: return cmp;
                default: return 0;
            }
        }

        void tick(const Rom& rom) {
            if (!alive) return;
            c20 -= tempo;
            if (c20 < 0) run(rom);
            if (!alive) return;
            c24 -= tempo;
            if (c24 < 0) {
                c24 = forever;
                if (sounding) {
                    v->release();
                    sounding = false;
                }
            }
        }

        void run(const Rom& rom) {
            for (int steps = 0; alive && steps < 10000; steps++) {
                uint32_t at = pc;
                uint32_t op = rom.at(at);
                int32_t a = (int32_t)rom.at(at + 1);
                pc = at + 2;
                if (op < 0x80) {
                    // A note (0: a rest); operand 0 returns from a call or ends the track.
                    if (a == 0) {
                        if (!calls.empty()) {
                            pc = calls.back();
                            calls.pop_back();
                            continue;
                        }
                        alive = false;
                        v->release();
                        return;
                    }
                    int key = -1;
                    if (op != 0) {
                        int n = trans + (int)op;
                        key = patch != 0 ? (int)rom.at(patch + (uint32_t)n) : (n > 0 && n < 128 ? n + 11 : 0);
                    }
                    int64_t dur = durations[a & 15];
                    if (a & 0x40) dur += dur >> 1;
                    c20 += dur;
                    if (a & 0x80) {
                        c24 = forever;
                    }
                    else {
                        c24 = c20 - (int64_t)tempo * 4;
                        if ((a & 0x30) == 0x10) c24 >>= 1;
                    }
                    if (key >= 0) {
                        v->velocity = std::clamp(vel + accents[(a >> 4) & 3], 0, 127);
                        v->volume = vol & 0xFF;
                        v->angle = pan & 0x1FF;
                        if (!(sounding && tie)) {
                            if (sample >= 0 && sample < 256) v->trigger(rom.records[(size_t)sample], key, cents);
                        }
                        else {
                            v->key = key;
                        }
                        sounding = true;
                        tie = (a & 0x80) != 0;
                    }
                    else {
                        tie = false;
                    }
                    return;
                }
                switch (op) {
                    case 0x80: tempo = a; break;
                    case 0x81: tempo += a; break;
                    case 0x82: case 0x88: vol = std::clamp(a, 0, 255); v->volume = vol; break;
                    case 0x83: vol = std::clamp(vol + a, 0, 255); v->volume = vol; break;
                    case 0x84: trans = a; break;
                    case 0x85: trans += a; break;
                    case 0x8D:
                        if (calls.size() <= 4) calls.push_back(pc);
                        pc = rom.at((uint32_t)a);
                        break;
                    case 0x8E:
                        if (loops.size() <= 4) loops.push_back({ pc, a });
                        break;
                    case 0x8F:
                        if (!loops.empty()) {
                            auto& l = loops.back();
                            if (l.second != 0 && --l.second == 0) {
                                loops.pop_back();
                                break;
                            }
                            pc = l.first;
                        }
                        break;
                    case 0x99: pc = (uint32_t)a; break;
                    case 0x9C: pan += a; break;
                    case 0xBF: pan = a; break;
                    case 0x9D: sample = a; break;
                    case 0xA2: vel = std::max(0, a); break;
                    case 0xA3: vel = std::clamp(vel + a, 0, 127); break;
                    case 0xA6: cents = a; break;
                    case 0xA7: cents += a; break;
                    case 0xA9: reg += a; cmp = reg; break;
                    case 0xAA: reg -= a; cmp = reg; break;
                    case 0xAB: reg &= a; cmp = reg; break;
                    case 0xAC: reg |= a; cmp = reg; break;
                    case 0xAD: reg ^= a; cmp = reg; break;
                    case 0xAE: pc = rom.at(at + 1 + (uint32_t)reg); break;
                    case 0xAF: pc = rom.at(at + 1 + (uint32_t)reg); reg++; cmp = reg; break;
                    case 0xB0:
                        if (a >= 6 && a <= 0x25) (*globals)[(size_t)(a - 6)] = reg;
                        else if (a == 0) vol = reg & 0xFF;
                        else if (a == 1) tempo = reg;
                        else if (a == 2) trans = reg;
                        else if (a == 4 || a == 5) cents = reg;
                        break;
                    case 0xB1:
                        if (a == 0) vol = std::clamp(vol + reg, 0, 255);
                        else if (a == 1) tempo += reg;
                        else if (a == 2) trans += reg;
                        else if (a == 4) cents -= reg;
                        break;
                    case 0xB2: reg = cmp = var(a); break;
                    case 0xB3: reg >>= std::clamp(a, 0, 31); break;
                    case 0xB4: reg = (int32_t)((uint32_t)reg << std::clamp(a, 0, 31)); break;
                    case 0xB5: case 0xB6: case 0xB7: case 0xB8: {
                        // op, variable, target (three dwords)
                        int32_t x = var(a);
                        uint32_t target = rom.at(pc);
                        bool take = op == 0xB5 ? x == 0 : op == 0xB6 ? x != 0 : op == 0xB7 ? x >= 0 : x < 0;
                        pc = take ? target : pc + 1;
                        break;
                    }
                    case 0xB9: cmp = reg - var(a); break;
                    case 0xBA: cmp = reg - a; break;
                    case 0xBC: patch = rom.at((uint32_t)a); break;
                    default: break;   // effects not ported (see the top)
                }
            }
        }
    };

    class RockDecoder : public rush2::audio2049::StreamDecoder {
    public:
        RockDecoder(std::shared_ptr<const Rom> r, const SongDef& def) : rom(std::move(r)) {
            frames = SIZE_MAX;
            rate = sample_rate;
            gain = 1.0f / out_scale;
            voices.resize(16);
            for (const TrackStart& t : def.tracks) {
                Track tr;
                tr.pc = t.pc;
                tr.v = &voices[(size_t)(t.channel & 15)];
                tr.globals = &globals;
                tracks.push_back(tr);
            }
        }

        size_t decode(int16_t* stereo, size_t count) override {
            size_t done = 0;
            while (done < count) {
                if (have == next) {
                    step();
                }
                size_t n = std::min(count - done, (size_t)(have - next));
                for (size_t k = 0; k < n * 2; k++) {
                    stereo[done * 2 + k] = (int16_t)std::clamp((int)std::lround(buf[(next * 2) + k] * out_scale),
                                                               -32768, 32767);
                }
                next += (int)n;
                done += n;
            }
            return done;
        }

    private:
        // The mix stays in 16 bits scaled by this (several voices at full level pass full scale); gain puts it back.
        static constexpr float out_scale = 0.2f;   // the loudest song peaks at 3.9 x full scale
        std::shared_ptr<const Rom> rom;
        std::vector<Voice> voices;
        std::vector<Track> tracks;
        std::array<int32_t, 32> globals{};
        std::array<float, block * 2> buf{};
        int have = 0, next = 0;

        uint32_t tick_clock = 0;    // sequencer time owed, in 1/sample_rate ticks

        void step() {
            // Voices render (and step their envelopes) per 64-sample block as ProjectR's mixer does; the sequencer
            // runs on its own 250 Hz clock, its ticks landing on the block they fall in.
            for (tick_clock += tick_rate * block; tick_clock >= sample_rate; tick_clock -= sample_rate) {
                for (Track& t : tracks) t.tick(*rom);
            }
            buf.fill(0.0f);
            for (Voice& v : voices) v.render(*rom, buf.data());
            have = block;
            next = 0;
        }
    };

    bool read_all(const std::filesystem::path& p, std::vector<uint8_t>& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return out.size() == 0x200000;
    }
}

const rush2::rock_music::SongInfo rush2::rock_music::songs[] = {
    { 27, "Golden Gate" }, { 25, "Embarcadero" }, { 31, "Market" }, { 41, "The Rock" }, { 29, "Downtown" },
    { 33, "The Heights" }, { 35, "Sunset" },
};
const int rush2::rock_music::song_count = (int)std::size(song_defs);

bool rush2::rock_music::load(const std::filesystem::path& data_dir) {
    {
        std::lock_guard lock{ rom_mutex };
        if (rom_data) return true;
    }
    std::vector<uint8_t> u[4];
    const char* names[4] = { "audio.u62", "audio.u61", "audio.u53", "audio.u49" };
    for (int i = 0; i < 4; i++) {
        if (!read_all(data_dir / names[i], u[i])) return false;
    }
    auto r = std::make_shared<Rom>();
    r->w.resize(rom_dwords);
    for (size_t half = 0; half < 2; half++) {
        const std::vector<uint8_t>& a = u[half * 2];
        const std::vector<uint8_t>& b = u[half * 2 + 1];
        for (size_t k = 0; k < 0x100000; k++) {
            r->w[half * 0x100000 + k] = a[2 * k] | (a[2 * k + 1] << 8) | (b[2 * k] << 16) | ((uint32_t)b[2 * k + 1] << 24);
        }
    }
    table(*r, 0xEFC963, r->semitones);
    table(*r, 0xEFC9AB, r->cents);
    table(*r, 0xEFCB74, r->volume);
    table(*r, 0xEFC8E3, r->velocity);
    table(*r, 0xEFCDB4, r->lobe);
    for (size_t i = 0; i < 256; i++) {
        uint32_t rec = r->w[0x7E5800 / 4 + i];
        uint32_t d[17];
        for (int k = 0; k < 17; k++) d[k] = r->at(rec + (uint32_t)k);
        Record& out = r->records[i];
        out.format = (int)d[1];
        out.rate = c3x(d[2]);
        out.root = (int32_t)d[10];
        out.attack = c3x(d[13]);
        out.release = c3x(d[16]);
        int32_t ls = (int32_t)d[7], le = (int32_t)d[8];
        out.loops = ls < le;
        out.loop_start = out.loops ? (uint32_t)ls : 0;
        out.loop_end = out.loops ? (uint32_t)le : 0;
        uint32_t n = std::min<uint32_t>(d[4], 0x800000);
        uint32_t start = d[3] - dsp_base;
        if (out.format == 3) {
            out.pcm.resize(n);
            for (uint32_t s = 0; s < n; s++) {
                uint32_t idx = start + s / 4;
                uint32_t word = idx < r->w.size() ? r->w[idx] : 0;
                out.pcm[s] = (float)(int8_t)(word >> (8 * (3 - s % 4))) * 256.0f;
            }
        }
        else if (out.format == 1) {
            out.pcm.resize(n);
            for (uint32_t s = 0; s < n; s++) {
                uint32_t idx = start + s / 2;
                uint32_t word = idx < r->w.size() ? r->w[idx] : 0;
                out.pcm[s] = (float)(int16_t)(s % 2 == 0 ? word >> 16 : word & 0xFFFF);
            }
        }
        if (out.loops && out.loop_end > out.pcm.size()) {
            out.loops = false;
        }
    }
    std::lock_guard lock{ rom_mutex };
    rom_data = std::move(r);
    return true;
}

bool rush2::rock_music::loaded() {
    std::lock_guard lock{ rom_mutex };
    return rom_data != nullptr;
}

std::unique_ptr<rush2::audio2049::StreamDecoder> rush2::rock_music::open(int n) {
    std::shared_ptr<const Rom> r;
    {
        std::lock_guard lock{ rom_mutex };
        r = rom_data;
    }
    if (!r || n < 0 || n >= song_count) return nullptr;
    return std::make_unique<RockDecoder>(r, song_defs[n]);
}
