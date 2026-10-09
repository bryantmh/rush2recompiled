// Rush 2049's music and sound effects: a host-side port of the MusyX sound system Rush 2049 uses.
//
// Rush 2049 (N64) runs Factor 5's MusyX (an N64 build of the 1.x runtime) on its own audio thread. The sequencer,
// synthesizer and macro interpreter here follow the decompiled MusyX runtime (github.com/AxioDL/musyx, MIT) and the
// N64 code in Rush 2049's boot segment; amuse (AxioDL, MIT) was the reference for the data formats. Where the N64 code
// differs from the GameCube runtime (volume maths, controller defaults, panning law) this file follows the N64 code,
// with the Rush 2049 addresses given below.
//
// Data (Rush 2049 ROM files, table at main 0x8011B5BC as in src/rush2049_rom.cpp; all big-endian except the ADSR
// tables). The sound files are LZ compressed; they are read here directly as LZ, because probing them with inflate
// first (as rom2049::read_file does) can make miniz expand without end (file 7):
// - File 6, project: groups {u32 size, u16 id, u16 type (0 song, 1 sfx), u32 offsets...}, offsets relative to the
//   group + 8. Song groups 0-11 (one per song): program pages {u16 macro, u8 priority, u8 max voices, u8 ff,
//   u8 program, u16 pad} (normal and drum tables, ffff-terminated) and MIDI setups {u16 song, u16 pad, 16 x
//   {program, volume, pan, reverb, chorus, 3 pad}}. Sfx groups 12-15: u16 count, u16 pad, count x {u16 id, u16 macro,
//   u8 priority, u8 max voices, u8 volume, u8 pan, u8 key, u8 volume group, u16 pad}; ids 0x00-0x76.
// - File 7, pool: u32 offsets of macros, tables, keymaps, layers. Macros {u32 size, u16 id, u16 pad, 8-byte steps}; a
//   step is two u32 words, opcode in the low 7 bits of the first, fields in its bytes 1-3 and the second word.
//   Tables {u32 size, u16 id, u16 pad, data}: all five are linear ADSRs (u16 attack ms, decay ms, sustain, release
//   ms, little-endian). Rush 2049 has no keymaps or layers.
// - File 8, sample directory: 0x1C-byte entries {u16 id, u16 pad, u32 offset into the sample data, u32, u8 root key,
//   u8 pad, u16 rate, u32 format << 24 | length, u32 loop start, u32 loop length (0 = one-shot)}.
// - Sample data: raw at ROM 0x39B40 (file 9). Every sample is format 3, Rush 2049's ADPCM: a 0x100-byte codebook (8
//   predictors of order 2, VADPCM layout) and 40-byte blocks of 64 samples, each block four s16 anchors (two per half)
//   and two 16-byte halves of 32 samples whose first byte holds predictor (high nibble) and shift. Samples are decoded
//   to PCM at load.
// - Files 10-21, songs 0-11 (MusyX song format with 16-bit delta times): header {u32 track table, u32 pattern table,
//   u32 track->channel map, u32 tempo track, u32 bpm (bit 30 = fractional), u32 loop tick}, 64 tracks of 12-byte
//   entries {u32 tick, u8 program, u8 volume, u16, u16 pattern (ffff end, fffe loop to entry n), s8 transpose, s8
//   velocity add}, patterns {u32, u32 pitch-bend stream, u32 modulation stream, events: u16 delta, u8 key, u8
//   velocity, then u16 length for notes; key bit 7 = program change (velocity 0) or controller (velocity bit 7)}.
//   384 ticks per beat. Songs 0-9 loop (every track ends in a loop entry), 10 and 11 end.
//
// Playback, per 1 ms step (MusyX runs 5 such steps per 5 ms frame): the sequencer advances, starting notes as
// macro voices (synthStartSound -> voice allocation -> macro) and keying them off when their length runs out; the
// macro interpreter runs every runnable voice; then voice jobs run: pitch (every 15 ms: note, detune, pitch bend,
// vibrato, sweeps, Doppler), volume (every 5 ms) and key events. Volume is the N64 integer chain (boot 0x8001ADA8):
// voice volume (velocity, envelopes) x volume-group pause and volume faders x master fader (music 21, sfx 22) x
// track volume x MIDI volume (CC7 only on N64), each step `fader * (v >> 7)`, then func_8001E0E0: 129-entry volume
// curve, pan curve {0, 0.7079, 1, 1} indexed by pan / 0x400000, surround the same, giving 15-bit left, right and
// surround gains. The hardware voice plays the PCM with pitch in 4.12 relative to 22050 Hz (Rush 2049's output rate),
// a linear ADSR stepped per millisecond and the gains ramped over 5 ms. Instead of mixing at 22050 Hz and resampling,
// each voice is resampled straight to the host rate with 4-point (Catmull-Rom) interpolation. The surround gain is
// matrixed into stereo as left + S, right - S (Dolby Surround style; music never uses it). A 15-bit gain of 32767
// comes out at 0.5 (output_gain: the RSP mixer itself isn't ported). Checked against the N64: the wing sound as Rush
// 2049 plays it (sfx 0x3D at volume 0.5, pan 0, pitch 0.75) gets voice gains 3568 / 3490 and step 1535/4096, the
// values src/wings_sound.cpp took from the running game.
//
// Rush 2049 calls (main segment): sndInit(22050 Hz, 32 voices, 16 music, 16 sfx) at 0x800A5F94, master volumes
// sndMasterVolume((int)(option / 10 * 127 * 0.9)) for music (func_800D6160) and sfx (func_800D6530), default options
// 10 -> 114. Songs: file 10 + n played with sndSeqPlay(group n, song n, data, no parameters) (func_800979A0).
// Effects: sndFXStart(id, default volume, default pan), then MIDI volume = (u8)(volume * 127), pan =
// (u8)((pan + 1) / 2 * 127), surround = (u8)((s + 1) / 2 * 127), Doppler = (u16)(pitch * 8192 - 1) (func_80097AFC,
// func_80097CA0).
//
// Not ported (unused by Rush 2049's data or calls): keymaps, layers, PlayMacro, portamento, traps and messages,
// variables, LFOs, tremolo, pitch ADSR, DLS ADSRs, curves (the curve ids the macros name don't exist), reverb and
// chorus sends (all zero), song sections and streaming. Channel 10 (MIDI 9) is the drum channel and Rush 2049 has no
// drum pages, so its notes are silent, as on the N64.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "audio2049.h"
#include "audio2049_dc.h"
#include "assets.h"

namespace {
    using u8 = uint8_t;
    using s8 = int8_t;
    using u16 = uint16_t;
    using s16 = int16_t;
    using u32 = uint32_t;
    using s32 = int32_t;
    using u64 = uint64_t;
    using s64 = int64_t;

    constexpr u32 mix_frq = 22050;
    constexpr u32 sample_data_rom = 0x39B40;
    constexpr u32 sample_data_size = 0x2D2A30;
    constexpr int voice_num = 32;
    constexpr int max_music = 16;
    constexpr int max_sfx = 16;
    constexpr u32 no_id = 0xFFFFFFFF;
    constexpr u8 fx_set = 0xFF;
    constexpr int master_music_fader = 21;
    constexpr int master_sfx_fader = 22;
    constexpr int default_master = 114; // (int)(10 / 10 * 127 * 0.9)
    // Output scale of a voice gain of 32767. The N64 RSP mixer ("N64 RSP mixer V1.1") isn't reverse engineered; with
    // Q15 gains (1.0) Rush 2049's songs would peak up to +6.7 dBFS and run at -11..-16 dBFS RMS, so the mixer evidently
    // keeps 6 dB of headroom: 0.5 puts the race songs at -17..-23 dBFS RMS, peaking near full scale.
    constexpr float output_gain = 0.5f;

    // Rush 2049 boot 0x8002CA40 / 0x8002CC44.
    constexpr float vol_tab[129] = {
        0.f, 3.0518499e-05f, 0.000152593f, 0.000396741f, 0.00070192601f, 0.00112918f, 0.001648f, 0.00222785f,
        0.0029297799f, 0.00372326f, 0.0046082898f, 0.00558489f, 0.0066530402f, 0.0078432597f, 0.0091250297f,
        0.0104984f, 0.0119633f, 0.0135502f, 0.0151982f, 0.0169988f, 0.0188604f, 0.0208441f, 0.0229194f,
        0.025116701f, 0.027405599f, 0.0298166f, 0.032319099f, 0.0349437f, 0.037659802f, 0.040467501f, 0.043427799f,
        0.046479698f, 0.049623098f, 0.052888598f, 0.056276102f, 0.059785798f, 0.063386902f, 0.067110203f,
        0.0709555f, 0.074922897f, 0.078981899f, 0.083162896f, 0.087466002f, 0.091921799f, 0.096469f, 0.101138f,
        0.10593f, 0.110843f, 0.115879f, 0.121036f, 0.12634701f, 0.13174801f, 0.13730299f, 0.142979f, 0.14877801f,
        0.15472899f, 0.160772f, 0.166997f, 0.173315f, 0.179785f, 0.186407f, 0.193121f, 0.200018f, 0.20700701f,
        0.21417899f, 0.22147299f, 0.228919f, 0.236488f, 0.24420901f, 0.252083f, 0.260079f, 0.26825801f, 0.276559f,
        0.28501201f, 0.29364899f, 0.30240801f, 0.31131899f, 0.32038301f, 0.32960001f, 0.33899999f, 0.34852099f,
        0.358226f, 0.36808401f, 0.37809399f, 0.38828701f, 0.398633f, 0.40913099f, 0.41981301f, 0.43064699f,
        0.44166401f, 0.45286399f, 0.46421701f, 0.47575301f, 0.48744199f, 0.499313f, 0.51139897f, 0.523606f,
        0.53602701f, 0.54863101f, 0.56141901f, 0.57438898f, 0.587542f, 0.60087901f, 0.61439902f, 0.62813199f,
        0.64201802f, 0.65614802f, 0.67043102f, 0.68492699f, 0.699637f, 0.71452999f, 0.72963703f, 0.74492598f,
        0.76042998f, 0.77614701f, 0.792077f, 0.808191f, 0.82454902f, 0.84109002f, 0.85784501f, 0.87484401f,
        0.89205599f, 0.90945202f, 0.927122f, 0.94500601f, 0.96307302f, 0.98141402f, 1.f, 1.f,
    };
    constexpr float pan_tab[4] = { 0.f, 0.7079f, 1.f, 1.f };


    // Rush 2049's per-track song table (0x8010FFD4, music option 12 "per track") for race tracks 1-6.
    constexpr int track_songs[19] = { 0, 1, 4, 2, 3, 7, 0, 1, 4, 2, 3, 7, 8, 9, 8, 8, 9, 9, 8 }; // 0x8010FFD4

    // ---------------------------------------------------------------------------------------------------------------
    // Data

    struct MStep {
        u32 p0, p1;
    };

    struct AdsrTable {
        u16 attack, decay, sustain, release;
    };

    struct Page {
        u16 macro = 0xFFFF;
        u8 prio = 0;
        u8 max_voices = 0;
    };

    struct MidiSetup {
        u8 program, volume, pan, reverb, chorus;
    };

    struct SongGroup {
        std::array<Page, 128> norm;
        std::array<Page, 128> drum;
        bool has_setup = false;
        std::array<MidiSetup, 16> setup{};
    };

    struct Fx {
        bool valid = false;
        u16 macro = 0;
        u8 prio = 0, max_voices = 0, volume = 127, pan = 64, key = 60, vgroup = 0;
    };

    struct Sample {
        u32 sinfo = 0; // root key << 24 | rate
        u32 length = 0;
        u32 loop_start = 0;
        u32 loop_length = 0;
        std::vector<s16> pcm;
    };

    struct Data {
        bool ok = false;
        std::unordered_map<u16, std::vector<MStep>> macros;
        std::unordered_map<u16, AdsrTable> adsr;
        std::unordered_map<u16, Sample> samples;
        std::array<SongGroup, 12> groups;
        std::array<Fx, 256> fx;
        std::array<std::vector<u8>, 12> songs;
    };

    u16 be16(const u8* p) {
        return u16((p[0] << 8) | p[1]);
    }
    u32 be32(const u8* p) {
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
    }
    u16 le16(const u8* p) {
        return u16(p[0] | (p[1] << 8));
    }

    s16 clamp16(s32 v) {
        return s16(std::clamp(v, -32768, 32767));
    }

    // Rush 2049's ADPCM (see src/wings_sound.cpp, which decodes one sample the same way).
    std::vector<s16> decode_sample(const u8* src, size_t avail, u32 length) {
        constexpr u32 block_size = 40;
        constexpr u32 block_samples = 64;
        u32 blocks = (length + block_samples - 1) / block_samples;
        if (avail < 0x100 + size_t(blocks) * block_size) {
            return {};
        }
        s16 book[8][2][8];
        for (int p = 0; p < 8; p++) {
            for (int order = 0; order < 2; order++) {
                for (int k = 0; k < 8; k++) {
                    book[p][order][k] = s16(be16(src + ((p * 2 + order) * 8 + k) * 2));
                }
            }
        }
        std::vector<s16> out;
        out.reserve(size_t(blocks) * block_samples);
        for (u32 b = 0; b < blocks; b++) {
            const u8* block = src + 0x100 + b * block_size;
            for (int half = 0; half < 2; half++) {
                const u8* data = block + 8 + half * 16;
                u8 header = data[0];
                const s16* b0 = book[header >> 4 & 7][0];
                const s16* b1 = book[header >> 4 & 7][1];
                int shift = header & 0xF;
                s32 e[32];
                for (int i = 0; i < 16; i++) {
                    for (int n = 0; n < 2; n++) {
                        s32 v = n == 0 ? (data[i] >> 4) : (data[i] & 0xF);
                        if (v >= 8) {
                            v -= 16;
                        }
                        v *= 1 << 12;
                        if (shift) {
                            v = (v * (0x8000 >> (shift - 1))) >> 16;
                        }
                        e[i * 2 + n] = v;
                    }
                }
                s16 s[32];
                s[0] = s16(be16(block + half * 4));
                s[1] = s16(be16(block + half * 4 + 2));
                auto run = [&](int first, int count) {
                    s32 prev2 = s[first - 2], prev1 = s[first - 1];
                    for (int k = 0; k < count; k++) {
                        s32 acc = b0[k] * prev2 + b1[k] * prev1 + 2048 * e[first + k];
                        for (int j = 0; j < k; j++) {
                            acc += b1[k - 1 - j] * e[first + j];
                        }
                        s[first + k] = clamp16(acc >> 11);
                    }
                };
                run(2, 6);
                run(8, 8);
                run(16, 8);
                run(24, 8);
                out.insert(out.end(), s, s + 32);
            }
        }
        out.resize(length);
        return out;
    }

    constexpr u32 main_rom = 0xB0CB10;
    constexpr u32 main_vram = 0x80086A50;
    constexpr u32 file_table_vram = 0x8011B5BC;
    constexpr int file_count = 183;

    // Rush 2049's LZSS (4 KB window, relative distances; rush2::wings::lz_decompress).
    bool lz_decompress(const u8* src, size_t src_size, std::vector<u8>& out) {
        out.clear();
        size_t pos = 0;
        while (pos < src_size) {
            u8 flags = src[pos++];
            for (int bit = 0; bit < 8; bit++) {
                if (flags & (1 << bit)) {
                    if (pos >= src_size) {
                        return false;
                    }
                    out.push_back(src[pos++]);
                    continue;
                }
                if (pos + 2 > src_size) {
                    return false;
                }
                u8 b0 = src[pos++];
                u8 b1 = src[pos++];
                u32 distance = ((b0 & 0xF0) << 4) | b1;
                u32 length = (b0 & 0x0F) + 2;
                if (distance == 0 && length == 2) {
                    return true;
                }
                for (u32 i = 0; i < length; i++) {
                    out.push_back(distance <= out.size() ? out[out.size() - distance] : 0);
                }
            }
        }
        return false;
    }

    // ROM offsets of files 0..file_count (the last is the main code).
    bool read_file_table(const std::vector<u8>& rom, std::vector<u32>& offsets) {
        std::vector<u8> main_code;
        if (rom.size() <= main_rom ||
            !rush2::assets::inflate_raw(rom.data() + main_rom, rom.size() - main_rom, main_code)) {
            return false;
        }
        offsets.clear();
        for (size_t pos = file_table_vram - main_vram; pos + 4 <= main_code.size(); pos += 4) {
            u32 offset = be32(&main_code[pos]);
            if (!offsets.empty() && (offset <= offsets.back() || offset >= main_rom)) {
                break;
            }
            offsets.push_back(offset);
        }
        if (offsets.size() != file_count) {
            return false;
        }
        offsets.push_back(main_rom);
        return true;
    }

    bool read_lz_file(const std::vector<u8>& rom, const std::vector<u32>& offsets, int index, std::vector<u8>& out) {
        u32 start = offsets[index], end = offsets[index + 1];
        return end <= rom.size() && start < end && lz_decompress(rom.data() + start, end - start, out);
    }

    bool parse(const std::vector<u8>& rom, Data& d) {
        std::vector<u8> proj, pool, sdir;
        std::vector<u32> offsets;
        if (!read_file_table(rom, offsets) || !read_lz_file(rom, offsets, 6, proj) ||
            !read_lz_file(rom, offsets, 7, pool) || !read_lz_file(rom, offsets, 8, sdir) ||
            offsets[9] != sample_data_rom || rom.size() < sample_data_rom + sample_data_size) {
            return false;
        }

        // Pool: macros and tables.
        if (pool.size() < 16) {
            return false;
        }
        u32 mac_off = be32(&pool[0]), tab_off = be32(&pool[4]), km_off = be32(&pool[8]);
        for (u32 o = mac_off; o + 8 <= tab_off && o + 8 <= pool.size();) {
            u32 size = be32(&pool[o]);
            if (size == 0xFFFFFFFF || size < 8 || o + size > pool.size()) {
                break;
            }
            std::vector<MStep>& steps = d.macros[be16(&pool[o + 4])];
            for (u32 s = o + 8; s + 8 <= o + size; s += 8) {
                steps.push_back({ be32(&pool[s]), be32(&pool[s + 4]) });
            }
            o += size;
        }
        for (u32 o = tab_off; o + 8 <= km_off && o + 8 <= pool.size();) {
            u32 size = be32(&pool[o]);
            if (size == 0xFFFFFFFF || size < 8 || o + size > pool.size()) {
                break;
            }
            if (size >= 16) {
                const u8* t = &pool[o + 8];
                d.adsr[be16(&pool[o + 4])] = { le16(t), le16(t + 2), le16(t + 4), le16(t + 6) };
            }
            o += size;
        }

        // Project: song and sfx groups.
        for (u32 o = 0; o + 0x28 <= proj.size();) {
            u32 end = be32(&proj[o]);
            if (end == 0xFFFFFFFF || end < 0x28 || o + end > proj.size()) {
                break;
            }
            u16 gid = be16(&proj[o + 4]);
            u16 type = be16(&proj[o + 6]);
            u32 sub = o + 8;
            u32 group_end = o + end;
            auto read_pages = [&](u32 p, std::array<Page, 128>& pages) {
                while (p + 8 <= group_end && be16(&proj[p]) != 0xFFFF) {
                    Page& pg = pages[proj[p + 5] & 0x7F];
                    pg.macro = be16(&proj[p]);
                    pg.prio = proj[p + 2];
                    pg.max_voices = proj[p + 3];
                    p += 8;
                }
            };
            if (type == 0 && gid < 12) {
                SongGroup& g = d.groups[gid];
                read_pages(sub + be32(&proj[o + 0x1C]), g.norm);
                read_pages(sub + be32(&proj[o + 0x20]), g.drum);
                for (u32 p = sub + be32(&proj[o + 0x24]); p + 4 + 16 * 8 <= group_end; p += 4 + 16 * 8) {
                    if (be16(&proj[p]) != gid) {
                        continue;
                    }
                    g.has_setup = true;
                    for (int c = 0; c < 16; c++) {
                        const u8* m = &proj[p + 4 + c * 8];
                        g.setup[c] = { m[0], m[1], m[2], m[3], m[4] };
                    }
                }
            }
            else if (type == 1) {
                u32 p = sub + be32(&proj[o + 0x1C]);
                if (p + 4 <= group_end) {
                    u16 count = be16(&proj[p]);
                    p += 4;
                    for (u16 i = 0; i < count && p + 12 <= group_end; i++, p += 12) {
                        u16 id = be16(&proj[p]);
                        if (id < d.fx.size()) {
                            Fx& fx = d.fx[id];
                            fx.valid = true;
                            fx.macro = be16(&proj[p + 2]);
                            fx.prio = proj[p + 4];
                            fx.max_voices = proj[p + 5];
                            fx.volume = proj[p + 6];
                            fx.pan = proj[p + 7];
                            fx.key = proj[p + 8];
                            fx.vgroup = proj[p + 9];
                        }
                    }
                }
            }
            o += end;
        }

        // Sample directory and data.
        const u8* data = rom.data() + sample_data_rom;
        for (size_t o = 0; o + 0x1C <= sdir.size(); o += 0x1C) {
            const u8* e = &sdir[o];
            u16 id = be16(e);
            if (id == 0xFFFF) {
                break;
            }
            u32 offset = be32(e + 4);
            u32 format = be32(e + 0x10) >> 24;
            if (format != 3 || offset >= sample_data_size) {
                continue;
            }
            Sample s;
            s.sinfo = (u32(e[0xC]) << 24) | be16(e + 0xE);
            s.length = be32(e + 0x10) & 0xFFFFFF;
            s.loop_start = be32(e + 0x14);
            s.loop_length = be32(e + 0x18);
            if (s.loop_length != 0 && (s.loop_start >= s.length || s.loop_start + s.loop_length > s.length)) {
                s.loop_length = 0;
            }
            s.pcm = decode_sample(data + offset, sample_data_size - offset, s.length);
            if (!s.pcm.empty()) {
                d.samples[id] = std::move(s);
            }
        }

        for (int i = 0; i < 12; i++) {
            if (!read_lz_file(rom, offsets, 10 + i, d.songs[i]) || d.songs[i].size() < 0x118) {
                return false;
            }
        }
        d.ok = !d.macros.empty() && !d.samples.empty();
        return d.ok;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Engine state

    enum class MacState : u8 { stopped, runnable, yielded };

    // cFlags bits (MusyX synth voice).
    constexpr u64 cf_break_on_start = 0x1;
    constexpr u64 cf_new = 0x2;
    constexpr u64 cf_wait_keyoff = 0x4;
    constexpr u64 cf_keyoff = 0x8;
    constexpr u64 cf_started = 0x10;
    constexpr u64 cf_start_pending = 0x20;
    constexpr u64 cf_keyoff_cmd = 0x80;
    constexpr u64 cf_adsr_set = 0x100;
    constexpr u64 cf_vibrato = 0x2000;
    constexpr u64 cf_vibrato_mod = 0x4000;
    constexpr u64 cf_envelope = 0x8000;
    constexpr u64 cf_pb_after_keyoff = 0x10000;
    constexpr u64 cf_wait_sample_end = 0x40000;
    constexpr u64 cf_pedal = 0x10000000000ull;
    constexpr u64 cf_keyoff_pedal = 0x40000000000ull;
    constexpr u64 cf_vol_select = 0x800000000ull;
    constexpr u64 cf_pan_select = 0x1000000000ull;
    constexpr u64 cf_pb_select = 0x2000000000ull;
    constexpr u64 cf_mod_select = 0x4000000000ull;
    constexpr u64 cf_span_select = 0x40000000ull;
    constexpr u64 cf_doppler_select = 0x80000000ull;

    struct CtrlSource {
        u8 ctrl;
        u8 combine;
        s32 scale; // 0x10000 = 1
    };

    struct CtrlDest {
        CtrlSource src[4];
        u8 num = 0;
        void set(u8 ctrl) {
            src[0] = { ctrl, 0, 0x10000 };
            num = 1;
        }
    };

    struct SVoice {
        u32 vid = no_id;
        const std::vector<MStep>* addr = nullptr;
        u32 cur = 0;
        u16 macro_id = 0;
        u16 alloc_id = 0;
        MacState mac_state = MacState::stopped;
        u64 wait = 0;
        u64 wait_time = 0;
        u64 mac_start_time = 0;
        u64 cflags = 0;
        bool fx = false;
        u8 setup_vol = 0, setup_pan = 64, setup_midi = 0, setup_set = 0, setup_track = 0xFF, setup_vgroup = 0;
        u8 midi = 0xFF, midi_set = 0xFF, track = 0xFF, vgroup = 0;
        u8 org_note = 0, last_note = 0;
        u16 cur_note = 0;
        s8 cur_detune = 0;
        u32 volume = 0, org_volume = 0;
        s32 env_current = 0, env_target = 0, env_delta = 0;
        u32 panning[2]{}, pan_target[2]{};
        s32 pan_time[2]{}, pan_delta[2]{};
        u32 vib_period = 0, vib_cur_time = 0;
        s32 vib_key_range = 0, vib_cent_range = 0, vib_mod_add_scale = 0;
        s32 vib_cur_offset = 0;
        u8 sweep_num[2]{};
        s32 sweep_cnt[2]{}, sweep_add[2]{}, sweep_off[2]{};
        u8 pb_lower = 2, pb_upper = 2;
        u16 pb_last = 0x2000;
        u32 age = 0, age_speed = 0;
        u8 prio = 0;
        u32 loop = 0;
        struct {
            const std::vector<MStep>* addr;
            u32 cur;
        } call_stack[4]{};
        u8 cs_index = 0, cs_num = 0;
        u32 sinfo = no_id;
        s32 play_frq = -1;
        CtrlDest inp_vol, inp_pan, inp_span, inp_pb, inp_doppler, inp_mod, inp_pedal;
        u64 last_low_call = 0, last_zero_call = 0;
        s64 job_low = -1, job_zero = -1, job_event = -1;
        bool counted = false; // in the music/sfx running counts
    };

    enum class AdsrState : u8 { start, attack, decay, sustain, release };

    struct AdsrVars {
        AdsrState state = AdsrState::start;
        u32 cnt = 0;
        s32 volume = 0; // 0x7fff0000 = 1
        s32 delta = 0;
        u16 attack = 0, decay = 0, sustain = 0x7FFF, release = 0;
    };

    // Hardware voice (the N64 RSP voice): sample playback, ADSR and gains.
    struct HwVoice {
        bool active = false;
        bool starting = false;
        bool end_after_ms = false;
        u32 owner = no_id; // synth voice vid it plays for
        bool fx = false;
        const Sample* smp = nullptr;
        double pos = 0.0;
        u16 pitch = 0; // 4.12 relative to 22050 Hz
        AdsrVars adsr;
        float adsr_from = 0.f, adsr_to = 0.f; // this millisecond's ADSR ramp (0-1)
        s16 vol_l = 0, vol_r = 0, vol_s = 0;  // 15-bit gains
        float ramp_from[2]{}, ramp_to[2]{};   // left/right gains (surround matrixed in)
        u32 ramp_pos = 0;
    };

    struct Tail {
        HwVoice v;
        float gain_l, gain_r;
        u32 left;
        u32 total;
    };

    struct Fader {
        s32 volume = 0x7F0000; // 16.16, 0..127
        s32 target = 0x7F0000;
        s32 delta = 0;
        bool active = false;
        s32 pause_vol = 0x7F0000;
        int seq_mode = 0; // 1 = stop the song when done
        int seq = -1;
    };

    struct Stream {
        u32 next = 0; // offset of the next value, 0 = none
        u16 value = 0;
        s16 next_delta = 0;
        u32 next_time = 0x7FFFFFFF;
    };

    struct SeqTrack {
        u32 base = 0, addr = 0;
    };

    struct SeqPattern {
        u32 l_time = 0, base_time = 0;
        u32 addr = 0; // 0 = no pattern
        u32 entry = 0;
        Stream pb, mod;
        u8 midi = 0;
    };

    struct SeqEvent {
        u32 time = 0;
        u8 type = 0;
        u32 addr = 0;
    };

    struct SeqNote {
        u32 vid;
        s32 end_time;
        u8 time_index;
    };

    struct Ticks {
        u32 low = 0;
        s32 high = 0;
    };

    struct Prg {
        u16 macro = 0xFFFF;
        u8 prio = 0, max_voices = 0;
    };

    struct Seq {
        bool active = false;
        int song = -1;
        const u8* arr = nullptr;
        size_t arr_size = 0;
        const SongGroup* group = nullptr;
        u8 set = 0; // midi set (sequencer index)
        u8 def_vgroup = 23;
        SeqTrack track[64];
        SeqPattern pattern[64];
        SeqEvent event[64];
        std::vector<u8> ev_list; // track ids ordered by event time
        std::vector<SeqNote> notes;
        Prg prg[16];
        u32 bpm = 0; // << 10
        u32 m_track = 0, m_addr = 0;
        Ticks tick_delta[2];
        Ticks time[2];
        u8 time_index = 0;
        u16 speed = 256;
        u32 loop_cnt = 0;
        bool loop_disable = false;
        u16 midi_priority[16];
    };

    struct Engine {
        Data data;
        SVoice sv[voice_num];
        HwVoice hw[voice_num];
        std::vector<Tail> tails;
        std::vector<int> free_list;
        int music_running = 0, fx_running = 0;
        u32 gen = 1;
        u8 midi_ctrl[8][16][134]{};
        u8 fx_ctrl[voice_num][134]{};
        u8 midi_last_note[8][16]{};
        u8 fx_last_note[voice_num]{};
        u8 pb_range[8][16]{};
        u8 fx_pb_range[voice_num]{};
        int last_started[9][16]{}; // voice index last started per channel (set 8 = fx)
        Fader faders[32];
        u8 track_volume[64]{};
        u64 synth_real_time = 0;
        u64 mac_real_time = 0;
        s64 job_tick = 0;
        u32 time_offset = 0;
        u64 step_count = 0;
        u32 rand_state = 0;
        Seq seq;
        // Mixer timing.
        u32 rate = 0;
        u32 ms_acc = 0;
        u32 ms_len = 0;
        u32 ms_left = 0;
        u32 ms_pos = 0;
        // Stats.
        uint32_t notes_started = 0, notes_dropped = 0, loops = 0;
        uint16_t channel_notes[16]{};
        u32 last_sfx_vid = no_id;
    };

    std::mutex engine_mutex;
    Engine* eng = nullptr; // allocated at load

    // ---------------------------------------------------------------------------------------------------------------
    // Utilities

    u16 snd_rand() {
        eng->rand_state *= 2822053219u;
        return u16(eng->rand_state >> 6);
    }

    s16 snd_sin(u32 angle) {
        // MusyX sndSin: 4096 units per turn, amplitude 4095.
        angle &= 0xFFF;
        return s16(std::lround(std::sin(angle * (2.0 * 3.14159265358979323846 / 4096.0)) * 4095.0));
    }

    // sndGetPitch: 4.12 playback step for key at the sample's root key and rate (synth_ac.c; 2049 boot 0x8001E4xx).
    u32 snd_get_pitch(u8 key, u32 sinfo) {
        if (sinfo == no_id) {
            sinfo = 0x40005622;
        }
        u8 okey = u8(sinfo >> 24);
        float frq = float(sinfo & 0xFFFFFF);
        if (key != okey) {
            frq *= std::pow(2.0f, (int(key) - int(okey)) / 12.0f);
        }
        return u32((4096.f * frq) / mix_frq);
    }

    s32 snd_pitch_up_one(u16 note) {
        return s32(note * 1.0594631f);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // MIDI controllers (snd_midictrl.c; N64 defaults from boot func_8002106C)

    void synth_key_state_update(SVoice& v);

    void set_cold_defaults(u8* c) {
        std::memset(c, 0, 134);
        c[7] = 0x7F;
        c[10] = 0x40;
        c[11] = 0x7F;
        c[98] = c[99] = c[100] = c[101] = 0x7F;
        c[128] = 0x40;
        c[132] = 0x40;
    }

    void set_warm_defaults(u8* c) {
        c[1] = 0;
        c[11] = 0x7F;
        c[33] = 0;
        c[43] = 0x7F;
        c[64] = c[65] = c[66] = c[67] = c[69] = 0;
        c[128] = 0x40;
    }

    u8* ctrl_array(u8 channel, u8 set) {
        return set != fx_set ? eng->midi_ctrl[set & 7][channel & 15] : eng->fx_ctrl[channel % voice_num];
    }

    void inp_reset_midi_ctrl(u8 channel, u8 set, bool cold) {
        u8* c = ctrl_array(channel, set);
        if (cold) {
            set_cold_defaults(c);
        }
        else {
            set_warm_defaults(c);
        }
        if (set != fx_set) {
            eng->midi_last_note[set & 7][channel & 15] = 0xFF;
        }
        else {
            eng->fx_last_note[channel % voice_num] = 0xFF;
        }
    }

    void inp_reset_channel_defaults(u8 channel, u8 set) {
        if (set != fx_set) {
            eng->pb_range[set & 7][channel & 15] = 2;
        }
        else {
            eng->fx_pb_range[channel % voice_num] = 2;
        }
    }

    u8& pb_range_ref(u8 channel, u8 set) {
        return set != fx_set ? eng->pb_range[set & 7][channel & 15] : eng->fx_pb_range[channel % voice_num];
    }

    void notify_channel(u8 channel, u8 set) {
        for (SVoice& v : eng->sv) {
            if (v.vid != no_id && v.midi_set == set && v.midi == channel) {
                synth_key_state_update(v);
            }
        }
    }

    void inp_set_rpn(u8 channel, u8 set, int op, u8 value) {
        u8* c = ctrl_array(channel, set);
        u16 rpn = u16(c[100] | (c[101] << 8));
        if (rpn != 0) {
            return;
        }
        u8& range = pb_range_ref(channel, set);
        if (op == 0) {
            range = value > 24 ? 24 : value;
        }
        else if (op == 2 && range != 0) {
            range--;
        }
        else if (op == 3 && range < 24) {
            range++;
        }
        else if (op == 1) {
            return;
        }
        for (SVoice& v : eng->sv) {
            if (v.vid != no_id && v.midi_set == set && v.midi == channel) {
                v.pb_lower = v.pb_upper = range;
            }
        }
    }

    void inp_set_midi_ctrl(u8 ctrl, u8 channel, u8 set, u8 value) {
        if (channel == 0xFF || ctrl >= 134) {
            return;
        }
        switch (ctrl) {
        case 6:
            inp_set_rpn(channel, set, 0, value);
            break;
        case 38:
            inp_set_rpn(channel, set, 1, value);
            break;
        case 96:
            inp_set_rpn(channel, set, 2, value);
            break;
        case 97:
            inp_set_rpn(channel, set, 3, value);
            break;
        }
        ctrl_array(channel, set)[ctrl] = value & 0x7F;
        notify_channel(channel, set);
    }

    void inp_set_midi_ctrl14(u8 ctrl, u8 channel, u8 set, u16 value) {
        if (channel == 0xFF) {
            return;
        }
        if (ctrl < 64) {
            inp_set_midi_ctrl(ctrl & 31, channel, set, u8(value >> 7));
            inp_set_midi_ctrl((ctrl & 31) + 32, channel, set, value & 0x7F);
        }
        else if (ctrl == 128 || ctrl == 129 || ctrl == 132 || ctrl == 133) {
            inp_set_midi_ctrl(ctrl & 254, channel, set, u8(value >> 7));
            inp_set_midi_ctrl((ctrl & 254) + 1, channel, set, value & 0x7F);
        }
        else {
            inp_set_midi_ctrl(ctrl, channel, set, u8(value >> 7));
        }
    }

    u16 inp_get_midi_ctrl(u8 ctrl, u8 channel, u8 set) {
        if (channel == 0xFF) {
            return 0;
        }
        const u8* c = ctrl_array(channel, set);
        if (ctrl < 0x40) {
            return u16(c[ctrl & 0x1F] << 7 | c[(ctrl & 0x1F) + 0x20]);
        }
        if (ctrl < 0x46) {
            return c[ctrl] < 0x40 ? 0 : 0x3FFF;
        }
        if (ctrl >= 0x60 && ctrl < 0x66) {
            return 0;
        }
        if (ctrl == 0x80 || ctrl == 0x81 || ctrl == 0x84 || ctrl == 0x85) {
            return u16(c[ctrl & 0xFE] << 7 | c[(ctrl & 0xFE) + 1]);
        }
        if (ctrl >= 134) {
            return 0;
        }
        return u16(c[ctrl] << 7);
    }

    u16 get_input_value(const SVoice& v, const CtrlDest& inp) {
        u32 value = 0;
        bool sign = false;
        for (u32 i = 0; i < inp.num; i++) {
            const CtrlSource& s = inp.src[i];
            u8 ctrl = s.ctrl;
            s32 tmp, vtmp;
            if (ctrl == 128 || ctrl == 1 || ctrl == 10 || ctrl == 160 || ctrl == 161 || ctrl == 131) {
                tmp = (ctrl == 160 || ctrl == 161) ? 0 : s32(inp_get_midi_ctrl(ctrl, v.midi, v.midi_set)) - 0x2000;
                tmp = s32((s64(tmp) * (s.scale >> 1)) >> 15);
                tmp = std::clamp(tmp, -0x2000, 0x1FFF);
                switch (s.combine & 15) {
                case 0:
                    value = u32(tmp + 0x2000);
                    sign = true;
                    break;
                case 1:
                    if (sign) {
                        vtmp = s32(value) + tmp - 0x2000;
                        value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    }
                    else {
                        value = u32(std::clamp(s32(value) + tmp, 0, 0x3FFF));
                    }
                    break;
                case 2:
                    if (sign) {
                        vtmp = ((s32(value) - 0x2000) * tmp) >> 13;
                    }
                    else {
                        vtmp = (tmp * s32(value)) >> 13;
                        sign = true;
                    }
                    value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    break;
                case 3:
                    if (sign) {
                        vtmp = (s32(value) - 0x2000) - tmp;
                        value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    }
                    else {
                        value = u32(std::clamp(s32(value) - tmp, 0, 0x3FFF));
                    }
                    break;
                }
            }
            else {
                switch (ctrl) {
                case 162:
                    tmp = v.org_note << 7;
                    break;
                case 163:
                    tmp = s32(v.org_volume >> 9);
                    break;
                case 164:
                    tmp = std::min<s32>(s32((eng->synth_real_time - v.mac_start_time) >> 8), 0x3FFF);
                    break;
                default:
                    tmp = inp_get_midi_ctrl(ctrl, v.midi, v.midi_set);
                    break;
                }
                tmp = s32((s64(tmp) * (s.scale >> 1)) >> 15);
                tmp = std::min(tmp, 0x3FFF);
                switch (s.combine & 15) {
                case 0:
                    value = u32(std::max(tmp, 0));
                    sign = false;
                    break;
                case 1:
                    if (sign) {
                        vtmp = s32(value) + tmp - 0x2000;
                        value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    }
                    else {
                        value = std::min<u32>(value + u32(std::max(tmp, 0)), 0x3FFF);
                    }
                    break;
                case 2:
                    if (sign) {
                        vtmp = (tmp * (s32(value) - 0x2000)) >> 14;
                        value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    }
                    else {
                        value = std::min<u32>((value * u32(std::max(tmp, 0))) >> 14, 0x3FFF);
                    }
                    break;
                case 3:
                    if (sign) {
                        vtmp = (s32(value) - 0x2000) - tmp;
                        value = u32(std::clamp(vtmp, -0x2000, 0x1FFF) + 0x2000);
                    }
                    else {
                        value = u32(std::clamp(s32(value) - tmp, 0, 0x3FFF));
                    }
                    break;
                }
            }
        }
        return u16(value);
    }

    u8 inp_translate_ex_ctrl(u8 ctrl) {
        switch (ctrl) {
        case 0x81:
            return 0x82;
        case 0x82:
            return 0xA0;
        case 0x83:
            return 0xA1;
        case 0x84:
            return 0x83;
        case 0x85:
            return 0x84;
        case 0x86:
            return 0xA2;
        case 0x87:
            return 0xA3;
        case 0x88:
            return 0xA4;
        default:
            return ctrl;
        }
    }

    void inp_init(SVoice& v) {
        // N64: one source each; volume is CC7 alone (no expression).
        v.inp_vol.set(7);
        v.inp_pan.set(10);
        v.inp_span.set(131);
        v.inp_pb.set(128);
        v.inp_doppler.set(132);
        v.inp_mod.set(1);
        v.inp_pedal.set(64);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Hardware voices

    bool adsr_change_state(AdsrVars& a) {
        switch (a.state) {
        case AdsrState::start:
            if ((a.cnt = a.attack) != 0) {
                a.state = AdsrState::attack;
                a.volume = 0;
                a.delta = 0x7FFF0000 / s32(a.attack);
                return false;
            }
            [[fallthrough]];
        case AdsrState::attack:
            if ((a.cnt = a.decay) != 0) {
                a.state = AdsrState::decay;
                a.volume = 0x7FFF0000;
                a.delta = -((0x7FFF0000 - s32(a.sustain) * 0x10000) / s32(a.decay));
                return false;
            }
            [[fallthrough]];
        case AdsrState::decay:
            if (a.sustain != 0) {
                a.state = AdsrState::sustain;
                a.volume = s32(a.sustain) << 16;
                a.delta = 0;
                return false;
            }
            [[fallthrough]];
        case AdsrState::release:
            break;
        default:
            return false;
        }
        a.volume = 0;
        return true;
    }

    bool adsr_setup(AdsrVars& a) {
        a.state = AdsrState::start;
        return adsr_change_state(a);
    }

    void adsr_release(AdsrVars& a) {
        a.state = AdsrState::release;
        a.cnt = a.release;
        if (a.release == 0) {
            a.cnt = 1;
            a.delta = 0;
            return;
        }
        a.delta = -(a.volume / s32(a.release));
    }

    // One millisecond of ADSR; returns true when the envelope has ended.
    bool adsr_handle(AdsrVars& a) {
        if (a.state == AdsrState::sustain) {
            return false;
        }
        a.volume += a.delta;
        if (--a.cnt == 0) {
            return adsr_change_state(a);
        }
        return false;
    }

    float adsr_level(const AdsrVars& a) {
        return float(std::clamp(a.volume >> 16, 0, 0x7FFF)) / 32767.0f;
    }

    void mac_sample_end_notify(SVoice& v);

    void hw_add_tail(HwVoice& h) {
        if (!h.active || h.starting || h.smp == nullptr || eng->rate == 0) {
            return;
        }
        u32 total = std::max<u32>(1, eng->rate / 500); // 2 ms
        u32 ramp_len = std::max<u32>(1, eng->rate / 200);
        float t = h.ramp_pos >= ramp_len ? 1.f : float(h.ramp_pos) / ramp_len;
        float a = h.adsr_to;
        Tail tail{ h, (h.ramp_from[0] + (h.ramp_to[0] - h.ramp_from[0]) * t) * a,
                   (h.ramp_from[1] + (h.ramp_to[1] - h.ramp_from[1]) * t) * a, total, total };
        eng->tails.push_back(tail);
    }

    void hw_deactivate(int i, bool notify, bool tail) {
        HwVoice& h = eng->hw[i];
        if (!h.active) {
            return;
        }
        if (tail) {
            hw_add_tail(h);
        }
        h.active = false;
        h.starting = false;
        h.end_after_ms = false;
        if (notify) {
            SVoice& v = eng->sv[i];
            if (v.vid != no_id && v.vid == h.owner) {
                mac_sample_end_notify(v);
            }
        }
    }

    bool hw_is_active(int i) {
        return eng->hw[i].active;
    }

    void hw_break(int i) {
        hw_deactivate(i, false, true);
    }

    void hw_set_pitch(int i, u32 pitch) {
        eng->hw[i].pitch = u16(std::min<u32>(pitch, 0x3FFF));
    }

    // N64 salCalcVolume (boot func_8001E0E0): 16.16 volume (0..127), pan and surround in 0..0x7F0000.
    float pan_interp(u32 p) {
        u32 i = p >> 22;
        float f = float(p & 0x3FFFFF) * (1.0f / 4194304.0f);
        return (1.f - f) * pan_tab[std::min<u32>(i, 3)] + f * pan_tab[std::min<u32>(i + 1, 3)];
    }

    void hw_set_volume(int i, u32 vol, u32 pan, u32 span) {
        HwVoice& h = eng->hw[i];
        if (vol >= 0x7F0001) {
            vol = 0x7F0000;
        }
        u32 vi = vol >> 16;
        float vf = float(vol & 0xFFFF) * (1.0f / 65536.0f);
        float f = (1.f - vf) * vol_tab[vi] + vf * vol_tab[vi + 1];
        s32 vs = s32(f * pan_interp(span) * 0.7079f * 32767.f);
        u32 span_m = 0x800000 - span;
        if (span_m >= 0x800000) {
            span_m = 0x7F0000;
        }
        float front = f * pan_interp(span_m);
        s32 vl, vr;
        if (pan == 0x800000) {
            vl = s32(front * 0.7079f * 32767.f);
            vr = s32(front * -0.7079f * 32767.f);
        }
        else {
            vr = s32(front * pan_interp(pan) * 32767.f);
            u32 pan_m = 0x800000 - pan;
            if (pan_m >= 0x800000) {
                pan_m = 0x7F0000;
            }
            vl = s32(front * pan_interp(pan_m) * 32767.f);
        }
        h.vol_l = s16(vl);
        h.vol_r = s16(vr);
        h.vol_s = s16(vs);
        // Matrix the surround channel into stereo (left + S, right - S) and ramp to the new gains.
        float new_l = (float(h.vol_l) + float(h.vol_s)) / 32768.f;
        float new_r = (float(h.vol_r) - float(h.vol_s)) / 32768.f;
        if (eng->rate != 0 && !h.starting && h.active) {
            u32 ramp_len = std::max<u32>(1, eng->rate / 200);
            float t = h.ramp_pos >= ramp_len ? 1.f : float(h.ramp_pos) / ramp_len;
            for (int c = 0; c < 2; c++) {
                h.ramp_from[c] = h.ramp_from[c] + (h.ramp_to[c] - h.ramp_from[c]) * t;
            }
            h.ramp_pos = 0;
        }
        else {
            h.ramp_from[0] = new_l;
            h.ramp_from[1] = new_r;
            h.ramp_pos = 0;
        }
        h.ramp_to[0] = new_l;
        h.ramp_to[1] = new_r;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Synth voices: allocation and jobs (synthvoice.c, synth.c)

    void voice_free(int i) {
        SVoice& v = eng->sv[i];
        if (v.vid == no_id) {
            return;
        }
        v.mac_state = MacState::stopped;
        v.addr = nullptr;
        v.prio = 0;
        v.vid = no_id;
        v.job_low = v.job_zero = v.job_event = -1;
        if (v.counted) {
            v.counted = false;
            if (v.fx) {
                eng->fx_running--;
            }
            else {
                eng->music_running--;
            }
        }
        eng->free_list.push_back(i);
    }

    // voiceAllocate, MusyX 1.x (synthvoice.c, versions <= 1.5.3).
    int voice_allocate(u8 priority, u8 max_voices, u16 alloc_id, bool fx) {
        bool type_alloc = fx ? (eng->fx_running >= max_sfx && voice_num > max_sfx)
                             : (eng->music_running >= max_music && voice_num > max_music);
        // Allocated voices ordered by priority (ascending), then index.
        int order[voice_num];
        int n = 0;
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].vid != no_id) {
                order[n++] = i;
            }
        }
        std::stable_sort(order, order + n, [](int a, int b) { return eng->sv[a].prio < eng->sv[b].prio; });

        int voice = -1;
        bool skip_alloc = (fx ? max_sfx : max_music) <= max_voices;
        if (!skip_alloc) {
            int num = 0;
            int k = 0;
            // Buckets with prio <= priority, until one yields a candidate of the same alloc id.
            while (k < n && eng->sv[order[k]].prio <= priority && voice == -1) {
                u8 p = eng->sv[order[k]].prio;
                for (; k < n && eng->sv[order[k]].prio == p; k++) {
                    SVoice& v = eng->sv[order[k]];
                    if (v.alloc_id != alloc_id) {
                        continue;
                    }
                    num++;
                    if (!type_alloc || fx == v.fx) {
                        if (v.cflags & cf_new) {
                            continue;
                        }
                        if (voice == -1 || v.age < eng->sv[voice].age) {
                            voice = order[k];
                        }
                    }
                }
            }
            if (num < max_voices) {
                for (; k < n && num < max_voices; k++) {
                    if (eng->sv[order[k]].alloc_id == alloc_id) {
                        num++;
                    }
                }
                skip_alloc = num < max_voices;
            }
        }
        if (skip_alloc) {
            voice = -1;
            if (!eng->free_list.empty() && !type_alloc) {
                voice = eng->free_list.front();
                eng->free_list.erase(eng->free_list.begin());
            }
            else {
                if (n == 0 || priority < eng->sv[order[0]].prio) {
                    return -1;
                }
                for (int k = 0; k < n && eng->sv[order[k]].prio <= priority && voice == -1;) {
                    u8 p = eng->sv[order[k]].prio;
                    for (; k < n && eng->sv[order[k]].prio == p; k++) {
                        SVoice& v = eng->sv[order[k]];
                        if (!type_alloc || fx == v.fx) {
                            if (v.cflags & cf_new) {
                                continue;
                            }
                            if (voice == -1 || eng->sv[voice].age > v.age) {
                                voice = order[k];
                            }
                        }
                    }
                }
                if (voice == -1) {
                    return -1;
                }
                if (eng->sv[voice].prio > priority) {
                    return -1;
                }
            }
        }
        if (voice == -1) {
            return -1;
        }
        SVoice& v = eng->sv[voice];
        if (v.vid != no_id && v.counted) {
            v.counted = false;
            if (v.fx) {
                eng->fx_running--;
            }
            else {
                eng->music_running--;
            }
        }
        auto it = std::find(eng->free_list.begin(), eng->free_list.end(), voice);
        if (it != eng->free_list.end()) {
            eng->free_list.erase(it);
        }
        if (fx) {
            eng->fx_running++;
        }
        else {
            eng->music_running++;
        }
        v.counted = true;
        return voice;
    }

    enum JobType { job_low, job_zero, job_event };

    void synth_add_job(SVoice& v, JobType type, u32 delta_time) {
        s64 due = eng->job_tick + delta_time / 256;
        switch (type) {
        case job_low:
            v.job_low = due;
            break;
        case job_zero:
            v.job_zero = due;
            break;
        case job_event:
            if (v.job_event < 0) {
                v.job_event = due;
            }
            break;
        }
    }

    void synth_key_state_update(SVoice& v) {
        synth_add_job(v, job_event, 0);
    }

    void synth_start_job_handling(SVoice& v) {
        v.last_low_call = eng->synth_real_time;
        v.last_zero_call = eng->synth_real_time;
        synth_add_job(v, job_low, 0);
        synth_add_job(v, job_zero, 0);
    }

    void synth_force_low_update(SVoice& v) {
        synth_add_job(v, job_low, 0);
        synth_add_job(v, job_zero, 0);
    }

    u32 ticks_per_second() {
        // synthSetBpm: ((bpm << 3) * 1536) / 240.
        u32 bpm = eng->seq.active ? (eng->seq.bpm >> 10) : 120;
        return std::max<u32>(1, ((bpm << 3) * 1536) / 240);
    }

    u32 convert_ticks(u32 t) {
        return u32(((u64(t) << 16) / ticks_per_second()) * 1000 / 32);
    }

    u32 convert_cents(const SVoice& v, u32 ccents) {
        u32 cpitch = snd_get_pitch(u8(std::min<u32>(ccents >> 16, 127)), v.sinfo) * 65536;
        u32 detune = ccents & 0xFFFF;
        if (detune != 0) {
            cpitch += detune * u32((snd_pitch_up_one(u16(cpitch >> 16)) & 0xFFFF) - (cpitch >> 16));
        }
        return cpitch;
    }

    void low_precision_handler(int i) {
        SVoice& v = eng->sv[i];
        if (!hw_is_active(i) && v.addr == nullptr) {
            return;
        }
        u32 dt = u32(eng->synth_real_time - v.last_low_call);
        v.last_low_call = eng->synth_real_time;

        if (v.cflags & cf_vibrato) {
            v.vib_cur_time += dt;
            if (v.vib_period / 256 != 0) {
                v.vib_cur_offset = snd_sin((v.vib_cur_time % v.vib_period * 16) / (v.vib_period / 256));
            }
        }
        if (v.sweep_num[0] | v.sweep_num[1]) {
            u32 cnt_delta = (dt << 8) >> 4;
            u32 add_factor = (dt << 4) >> 4;
            for (int j = 0; j < 2; j++) {
                if (v.sweep_num[j] == 0) {
                    continue;
                }
                v.sweep_cnt[j] -= s32(cnt_delta);
                if (v.sweep_cnt[j] <= 0) {
                    v.sweep_cnt[j] = s32(v.sweep_num[j]) << 16;
                    v.sweep_off[j] = 0;
                }
                else {
                    v.sweep_off[j] += (v.sweep_add[j] >> 12) * s32(add_factor);
                }
            }
        }
        for (int j = 0; j < 2; j++) {
            if (v.panning[j] == v.pan_target[j]) {
                continue;
            }
            v.pan_time[j] -= s32(dt);
            if (v.pan_time[j] <= 0) {
                v.panning[j] = v.pan_target[j];
                v.pan_time[j] = 0;
            }
            else {
                s32 p = s32(v.pan_target[j]) - (v.pan_time[j] / 256) * v.pan_delta[j];
                v.panning[j] = u32(std::clamp(p, 0, 0x7F0000));
            }
        }

        u32 ccents = u32(v.cur_note) * 65536 + u32((v.cur_detune * 65536) / 100);
        s32 pbend;
        if ((v.cflags & (cf_pb_after_keyoff | cf_started | cf_start_pending)) != 0 && v.midi != 0xFF) {
            pbend = get_input_value(v, v.inp_pb);
            v.pb_last = u16(pbend);
        }
        else {
            pbend = v.pb_last;
        }
        if (pbend != 0x2000) {
            pbend -= 0x2000;
            if (pbend < 0) {
                ccents += u32(s32(v.pb_lower) * pbend * 8);
            }
            else {
                ccents += u32(s32(v.pb_upper) * pbend * 8);
            }
        }
        if (v.cflags & cf_vibrato) {
            u16 modulation = get_input_value(v, v.inp_mod);
            s32 vrange = v.vib_key_range * 256 + (v.vib_cent_range * 256) / 100;
            if (v.vib_mod_add_scale != 0) {
                vrange += (v.vib_mod_add_scale * ((modulation >> 7) & 0x1FF)) >> 7;
            }
            s32 voff = (v.cflags & cf_vibrato_mod) ? (v.vib_cur_offset * ((modulation >> 7) & 0x1FF)) >> 7
                                                   : v.vib_cur_offset;
            ccents += u32((vrange * voff) >> 4);
        }
        u32 cpitch = convert_cents(v, ccents);
        cpitch += u32(v.sweep_off[0] + v.sweep_off[1]);
        cpitch = ((cpitch >> 16) * get_input_value(v, v.inp_doppler)) >> 13;
        hw_set_pitch(i, cpitch);
        synth_add_job(v, job_low, 0xF00);
    }

    // ZeroOffsetHandler with the N64 volume chain (boot 0x8001ADA8).
    void zero_offset_handler(int i) {
        SVoice& v = eng->sv[i];
        if (!hw_is_active(i) && v.addr == nullptr) {
            return;
        }
        u32 dt = u32(eng->synth_real_time - v.last_zero_call);
        v.last_zero_call = eng->synth_real_time;

        if (v.cflags & cf_envelope) {
            v.env_current += v.env_delta * s32(dt >> 8);
            if (v.env_delta < 0) {
                if (v.env_target >= v.env_current) {
                    v.env_current = v.env_target;
                    v.cflags &= ~cf_envelope;
                }
            }
            else if (v.env_target <= v.env_current) {
                v.env_current = v.env_target;
                v.cflags &= ~cf_envelope;
            }
            v.volume = u32(std::max(v.env_current, 0));
        }

        const Fader& group = eng->faders[v.vgroup & 31];
        const Fader& master = eng->faders[v.fx ? master_sfx_fader : master_music_fader];
        u32 vol = v.volume;
        vol = u32(group.pause_vol >> 16) * (vol >> 7);
        vol = u32(group.volume >> 16) * (vol >> 7);
        vol = u32(master.volume >> 16) * (vol >> 7);
        if (v.track != 0xFF) {
            vol = eng->track_volume[v.track & 63] * (vol >> 7);
        }
        u32 pan = v.panning[0];
        u32 span = 0;
        if (v.midi != 0xFF) {
            vol = u32(get_input_value(v, v.inp_vol) >> 7) * (vol >> 7);
            if (pan != 0x800000) {
                s32 p = s32(pan) + ((s32(get_input_value(v, v.inp_pan) >> 7) - 0x40) << 16);
                pan = u32(std::clamp(p, 0, 0x7F0000));
            }
            span = std::min<u32>(u32(get_input_value(v, v.inp_span)) << 9, 0x7F0000);
        }
        hw_set_volume(i, vol, pan, span);

        if (v.age != 0) {
            s64 a = s64(v.age) - s64(v.age_speed) * dt;
            v.age = a < 0 ? 0 : u32(a);
        }
        synth_add_job(v, job_zero, (5 - eng->time_offset) * 256);
    }

    void hw_start(int i);
    void hw_key_off(int i);
    void mac_set_pedal_state(SVoice& v, bool state);

    void event_handler(int i) {
        SVoice& v = eng->sv[i];
        if (!hw_is_active(i) && v.addr == nullptr) {
            return;
        }
        mac_set_pedal_state(v, get_input_value(v, v.inp_pedal) > 0x1F80);
        if (v.cflags & cf_start_pending) {
            v.cflags &= ~cf_start_pending;
            v.cflags |= cf_started;
            hw_start(i);
        }
        if ((v.cflags & (cf_pedal | cf_keyoff_cmd | cf_started)) == (cf_keyoff_cmd | cf_started)) {
            v.cflags &= ~(cf_keyoff_cmd | cf_started);
            hw_key_off(i);
        }
    }

    void handle_voices() {
        s64 t = eng->job_tick;
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].job_low >= 0 && eng->sv[i].job_low <= t) {
                eng->sv[i].job_low = -1;
                low_precision_handler(i);
            }
        }
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].job_event >= 0 && eng->sv[i].job_event <= t) {
                eng->sv[i].job_event = -1;
                event_handler(i);
            }
        }
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].job_zero >= 0 && eng->sv[i].job_zero <= t) {
                eng->sv[i].job_zero = -1;
                zero_offset_handler(i);
            }
        }
    }

    void voice_set_last_started(SVoice& v, int i) {
        eng->last_started[v.midi_set == fx_set ? 8 : (v.midi_set & 7)][v.midi & 15] = i;
    }

    bool voice_is_last_started(const SVoice& v, int i) {
        if (v.vid == no_id || v.midi == 0xFF) {
            return false;
        }
        return eng->last_started[v.midi_set == fx_set ? 8 : (v.midi_set & 7)][v.midi & 15] == i;
    }

    void inp_set_midi_last_note(u8 midi, u8 set, u8 key) {
        if (midi == 0xFF) {
            return;
        }
        if (set != fx_set) {
            eng->midi_last_note[set & 7][midi & 15] = key;
        }
        else {
            eng->fx_last_note[midi % voice_num] = key;
        }
    }

    u8 inp_get_midi_last_note(u8 midi, u8 set) {
        if (midi == 0xFF) {
            return 0xFF;
        }
        return set != fx_set ? eng->midi_last_note[set & 7][midi & 15] : eng->fx_last_note[midi % voice_num];
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Hardware voice commands

    void hw_init_sample_playback(int i, const Sample* smp, u32 offset, bool set_default_adsr, u32 owner, bool fx) {
        HwVoice& h = eng->hw[i];
        if (h.active) {
            hw_add_tail(h);
        }
        h.active = true;
        h.starting = true;
        h.end_after_ms = false;
        h.owner = owner;
        h.fx = fx;
        h.smp = smp;
        h.pos = offset;
        if (set_default_adsr) {
            h.adsr.attack = 0;
            h.adsr.decay = 0;
            h.adsr.sustain = 0x7FFF;
            h.adsr.release = 0;
        }
        h.adsr_from = h.adsr_to = 0.f;
    }

    void hw_start(int i) {
        (void)i; // the voice starts at the next hardware step, once cf_start_pending is clear (hw_prepare_ms)
    }

    void hw_key_off(int i) {
        HwVoice& h = eng->hw[i];
        if (h.active && !h.starting) {
            adsr_release(h.adsr);
        }
        else if (h.active) {
            h.end_after_ms = true; // released before it started: the default ADSR ends at once
        }
    }

    void hw_set_adsr(int i, const AdsrTable& t) {
        HwVoice& h = eng->hw[i];
        h.adsr.attack = t.attack;
        h.adsr.decay = t.decay;
        h.adsr.sustain = u16(std::min<u32>(u32(t.sustain) << 3, 0x7FFF));
        h.adsr.release = t.release;
        if (h.active && !h.starting && adsr_setup(h.adsr)) {
            h.end_after_ms = true;
        }
    }

    // Per-millisecond hardware step, after the synth ran: start new voices and step the ADSRs.
    void hw_prepare_ms() {
        for (int i = 0; i < voice_num; i++) {
            HwVoice& h = eng->hw[i];
            if (!h.active) {
                continue;
            }
            if (h.end_after_ms) {
                hw_deactivate(i, true, false);
                continue;
            }
            if (h.starting) {
                // Starts only once the synth has issued hwStart (cf_started) or the voice was keyed off.
                SVoice& v = eng->sv[i];
                if (v.vid == h.owner && (v.cflags & cf_start_pending)) {
                    continue;
                }
                h.starting = false;
                h.ramp_from[0] = h.ramp_to[0];
                h.ramp_from[1] = h.ramp_to[1];
                h.ramp_pos = 0;
                if (adsr_setup(h.adsr)) {
                    hw_deactivate(i, true, false);
                    continue;
                }
                h.adsr_from = adsr_level(h.adsr);
            }
            else {
                h.adsr_from = adsr_level(h.adsr);
            }
            bool done = adsr_handle(h.adsr);
            h.adsr_to = adsr_level(h.adsr);
            if (done) {
                h.end_after_ms = true;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Macros (synthmacros.c)

    void mac_make_active(SVoice& v);
    void mac_make_inactive(SVoice& v, MacState state);

    const std::vector<MStep>* data_get_macro(u16 id) {
        auto it = eng->data.macros.find(id);
        return it == eng->data.macros.end() || it->second.empty() ? nullptr : &it->second;
    }

    void un_yield_macro(SVoice& v, bool disable_update) {
        if (v.wait != 0) {
            if (!disable_update) {
                synth_force_low_update(v);
            }
            v.wait = 0;
            v.wait_time = eng->mac_real_time;
            v.cflags &= ~(cf_wait_keyoff | cf_wait_sample_end);
        }
    }

    void mac_make_active(SVoice& v) {
        if (v.mac_state == MacState::runnable || v.addr == nullptr) {
            return;
        }
        un_yield_macro(v, false);
        v.mac_state = MacState::runnable;
    }

    void mac_make_inactive(SVoice& v, MacState state) {
        if (v.mac_state == state) {
            return;
        }
        if (state == MacState::stopped) {
            un_yield_macro(v, true);
        }
        v.mac_state = state;
    }

    void mac_sample_end_notify(SVoice& v) {
        if (v.mac_state != MacState::yielded || v.addr == nullptr) {
            return;
        }
        if (v.cflags & cf_wait_sample_end) {
            mac_make_active(v);
        }
    }

    void mac_set_external_keyoff(SVoice& v) {
        v.cflags |= cf_keyoff;
        if (v.addr == nullptr) {
            return;
        }
        if (!(v.cflags & cf_pedal)) {
            if (v.cflags & cf_wait_keyoff) {
                mac_make_active(v);
            }
        }
        else {
            v.cflags |= cf_keyoff_pedal;
        }
    }

    void mac_set_pedal_state(SVoice& v, bool state) {
        if (state) {
            v.cflags |= cf_pedal;
        }
        else {
            if (v.addr && (v.cflags & cf_keyoff_pedal) && (v.cflags & cf_wait_keyoff)) {
                mac_make_active(v);
            }
            v.cflags &= ~(cf_pedal | cf_keyoff_pedal);
        }
    }

    int voice_index(const SVoice& v) {
        return int(&v - eng->sv);
    }

    u32 mcmd_end(SVoice& v) {
        voice_free(voice_index(v));
        return 1;
    }

    u32 mcmd_wait(SVoice& v, MStep& s) {
        u32 ms = u16(s.p1 >> 16);
        if (ms == 0) {
            return 0;
        }
        int i = voice_index(v);
        if (u8(s.p0 >> 8) & 1) {
            if (v.cflags & cf_keyoff) {
                if (!(v.cflags & cf_pedal)) {
                    return 0;
                }
                v.cflags |= cf_keyoff_pedal;
            }
            v.cflags |= cf_wait_keyoff;
        }
        else {
            v.cflags &= ~cf_wait_keyoff;
        }
        if (u8(s.p0 >> 24) & 1) {
            if (!(v.cflags & cf_start_pending) && !hw_is_active(i)) {
                return 0;
            }
            v.cflags |= cf_wait_sample_end;
        }
        else {
            v.cflags &= ~cf_wait_sample_end;
        }
        if (u8(s.p0 >> 16) & 1) {
            ms = snd_rand() % ms;
        }
        if (ms != 0xFFFF) {
            bool w = (u8(s.p1 >> 8) & 1) != 0;
            u32 t = w ? ms * 256 : convert_ticks(ms);
            if (w) {
                v.wait = (u8(s.p1) & 1) ? v.mac_start_time + t : eng->mac_real_time + t;
            }
            else {
                v.wait = (u8(s.p1) & 1) ? t : v.wait_time + t;
            }
            if (!(v.wait > eng->mac_real_time)) {
                v.wait_time = v.wait;
                v.wait = 0;
            }
        }
        else {
            v.wait = ~u64(0);
        }
        if (v.wait != 0) {
            mac_make_inactive(v, MacState::yielded);
            return 1;
        }
        return 0;
    }

    void jump_to(SVoice& v, u16 macro, u16 step) {
        if (const std::vector<MStep>* m = data_get_macro(macro)) {
            v.addr = m;
            v.cur = step;
        }
    }

    void mcmd_set_key_common(SVoice& v) {
        if (voice_is_last_started(v, voice_index(v))) {
            inp_set_midi_last_note(v.midi, v.midi_set, u8(v.cur_note));
        }
    }

    void do_set_pitch(SVoice& v) {
        static const u16 kf[13] = { 4096, 4339, 4597, 4871, 5160, 5467, 5792, 6137, 6502, 6888, 7298, 7732, 8192 };
        u32 frq = u32(v.play_frq) & 0xFFFFFF;
        u32 ofrq = v.sinfo & 0xFFFFFF;
        if (ofrq == 0 || frq == 0) {
            return;
        }
        if (ofrq == frq) {
            v.cur_note = u8(v.sinfo >> 24);
            v.cur_detune = 0;
            return;
        }
        bool up = ofrq < frq;
        u32 f = up ? (frq << 12) / ofrq : (ofrq << 12) / frq;
        u32 of = f >> 12;
        u32 no;
        for (no = 0; no < 11; no++) {
            if (of < (1u << (no + 1))) {
                break;
            }
        }
        f /= (1u << no);
        int i;
        for (i = 11; i > 0; i--) {
            if (f > kf[i]) {
                break;
            }
        }
        if (up) {
            v.cur_note = u16((v.sinfo >> 24) + no * 12 + i);
            v.cur_detune = s8(((f - kf[i]) * 100) / (kf[i + 1] - kf[i]));
        }
        else {
            s32 key = i + s32(no) * 12;
            u8 okey = u8(v.sinfo >> 24);
            if (key > okey) {
                v.cur_note = 0;
                v.cur_detune = 0;
            }
            else {
                v.cur_note = u16(okey - key);
                v.cur_detune = s8(((kf[i] - std::min<u32>(f, kf[i])) * 100) / (kf[i + 1] - kf[i]));
            }
        }
    }

    void mcmd_start_sample(SVoice& v, MStep& s) {
        u16 id = u16(s.p0 >> 8);
        auto it = eng->data.samples.find(id);
        if (it == eng->data.samples.end()) {
            return;
        }
        const Sample& smp = it->second;
        u32 offset;
        switch (u8(s.p0 >> 24)) {
        case 0:
            offset = s.p1;
            break;
        case 1:
            offset = (u8(0x7F - (v.volume >> 16)) * s.p1) / 0x7F;
            break;
        case 2:
            offset = (u8(v.volume >> 16) * s.p1) / 0x7F;
            break;
        default:
            offset = 0;
            break;
        }
        if (offset >= smp.length) {
            offset = smp.length - 1;
        }
        int i = voice_index(v);
        hw_init_sample_playback(i, &smp, offset, !(v.cflags & cf_adsr_set), v.vid, v.fx);
        v.sinfo = smp.sinfo;
        if (v.play_frq != -1) {
            do_set_pitch(v);
        }
        v.cflags |= cf_start_pending;
        synth_key_state_update(v);
    }

    void tvol_envelope(SVoice& v, MStep& s, s32 start_vol) {
        u32 time = u16(s.p1 >> 16);
        time = (u8(s.p1 >> 8) & 1) ? time * 256 : convert_ticks(time);
        s32 mstime = s32(time / 256);
        if (mstime == 0) {
            mstime = 1;
        }
        u32 tvol = (v.volume * u8(s.p0 >> 8)) >> 7;
        tvol += u32(u8(s.p0 >> 16)) << 16;
        if (tvol > 0x7F0000) {
            tvol = 0x7F0000;
        }
        // Curves (para0 >> 24 | para1 << 8) don't exist in Rush 2049's pool: volume stays linear.
        v.env_target = s32(tvol);
        v.env_current = start_vol;
        v.env_delta = (s32(tvol) - start_vol) / mstime;
        v.volume = u32(start_vol);
        v.cflags |= cf_envelope;
    }

    void select_source(SVoice& v, CtrlDest& dest, MStep& s, u64 flag) {
        u8 comb;
        if (!(v.cflags & flag)) {
            comb = 0;
            v.cflags |= flag;
        }
        else {
            comb = u8(s.p1);
        }
        s32 scale = (s32(s16(s.p0 >> 16)) << 16) / 100;
        if (scale < 0) {
            scale -= (s32(s8(s.p1 >> 16)) << 8) / 100;
        }
        else {
            scale += (s32(s8(s.p1 >> 16)) << 8) / 100;
        }
        if (comb == 0) {
            dest.num = 0;
        }
        if (dest.num < 4 && (u8(s.p1 >> 8) == 0)) { // variables are not ported
            dest.src[dest.num++] = { inp_translate_ex_ctrl(u8(s.p0 >> 8)), comb, scale };
        }
    }

    void voice_set_priority(SVoice& v, u8 prio) {
        v.prio = prio;
    }

    void mac_handle_active(int i) {
        SVoice& v = eng->sv[i];
        if (v.cflags & (cf_break_on_start | cf_new)) {
            if (v.cflags & cf_break_on_start) {
                v.cflags &= ~cf_break_on_start;
                hw_break(i);
            }
            v.panning[0] = v.pan_target[0] = u32(v.setup_pan) << 16;
            v.panning[1] = v.pan_target[1] = 0;
            v.volume = u32(v.setup_vol) << 16;
            v.org_volume = v.volume;
            v.midi = v.setup_midi;
            v.midi_set = v.setup_set;
            v.track = v.setup_track;
            v.vgroup = v.setup_vgroup;
            v.vib_mod_add_scale = 0;
            inp_init(v);
            u8 last = inp_get_midi_last_note(v.midi, v.midi_set);
            v.last_note = last != 0xFF ? last : v.org_note;
            inp_set_midi_last_note(v.midi, v.midi_set, v.org_note);
            voice_set_last_started(v, i);
            u8 range = v.midi != 0xFF ? pb_range_ref(v.midi, v.midi_set) : 2;
            v.pb_lower = v.pb_upper = range;
            v.loop = 0;
            v.sweep_num[0] = v.sweep_num[1] = 0;
            v.sweep_off[0] = v.sweep_off[1] = 0;
            v.sinfo = no_id;
            v.play_frq = -1;
            v.pb_last = 0x2000;
            v.cflags &= cf_keyoff;
            v.wait_time = eng->mac_real_time;
            v.mac_start_time = eng->mac_real_time;
            synth_start_job_handling(v);
        }

        for (int steps = 0; steps < 32; steps++) {
            if (v.addr == nullptr || v.cur >= v.addr->size()) {
                mcmd_end(v);
                return;
            }
            MStep s = (*v.addr)[v.cur++];
            u32 ex = 0;
            switch (s.p0 & 0x7F) {
            case 0x00: // End
            case 0x01: // Stop
                ex = mcmd_end(v);
                break;
            case 0x02: // SplitKey
                if (v.cur_note >= u8(s.p0 >> 8)) {
                    jump_to(v, u16(s.p0 >> 16), u16(s.p1));
                }
                break;
            case 0x03: // SplitVel
                if (u8(v.volume >> 16) >= u8(s.p0 >> 8)) {
                    jump_to(v, u16(s.p0 >> 16), u16(s.p1));
                }
                break;
            case 0x04: // WaitTicks
                ex = mcmd_wait(v, s);
                break;
            case 0x05: { // Loop
                bool skip = false;
                if (v.loop == 0) {
                    v.loop = (u8(s.p0 >> 16) & 1) ? (snd_rand() % std::max<u32>(1, u16(s.p1 >> 16))) : (s.p1 >> 16);
                    if (v.loop == 0xFFFF) {
                        skip = true;
                    }
                    else {
                        ++v.loop;
                    }
                }
                else if (v.loop == 0xFFFF) {
                    skip = true;
                }
                if (!skip && --v.loop == 0) {
                    break;
                }
                if ((u8(s.p0 >> 8) & 1) && (v.cflags & (cf_pedal | cf_keyoff)) == cf_keyoff) {
                    v.loop = 0;
                }
                else if ((u8(s.p0 >> 24) & 1) && !(v.cflags & cf_start_pending) && !hw_is_active(i)) {
                    v.loop = 0;
                }
                else {
                    v.cur = u16(s.p1);
                }
                break;
            }
            case 0x06: // Goto
                if (const std::vector<MStep>* m = data_get_macro(u16(s.p0 >> 16))) {
                    v.addr = m;
                    v.cur = u16(s.p1);
                }
                else {
                    ex = mcmd_end(v);
                }
                break;
            case 0x07: { // WaitMs
                MStep t = s;
                t.p1 = (t.p1 & ~0xFF00u) | 0x100u;
                ex = mcmd_wait(v, t);
                break;
            }
            case 0x09: { // SendKeyOff
                u32 base = ((u32(v.org_note) + u8(s.p0 >> 8)) & 0xFF);
                u16 macro = u16(s.p0 >> 16);
                for (SVoice& o : eng->sv) {
                    if (o.vid != no_id && o.macro_id == macro && o.org_note == base) {
                        mac_set_external_keyoff(o);
                    }
                }
                break;
            }
            case 0x0C: { // SetAdsr
                u16 table = u16(s.p0 >> 8);
                auto it = eng->data.adsr.find(table);
                if (it != eng->data.adsr.end() && u8(s.p0 >> 24) == 0) {
                    hw_set_adsr(i, it->second);
                    v.cflags |= cf_adsr_set;
                }
                break;
            }
            case 0x0D: { // ScaleVolume
                u32 scale = u8(s.p0 >> 8);
                if (u8(s.p1 >> 8) == 0) {
                    v.volume = (v.volume * scale) / 0x7F;
                }
                else {
                    v.volume = (v.org_volume * scale) / 0x7F;
                }
                v.volume += u32(u8(s.p0 >> 16)) << 16;
                if (v.volume > 0x7F0000) {
                    v.volume = 0x7F0000;
                }
                break;
            }
            case 0x0E: { // Panning
                u32 width = u16(s.p0 >> 16);
                v.pan_time[0] = s32(width * 256);
                s32 mstime = s8(s.p1);
                v.panning[0] = u32(u8(s.p0 >> 8)) << 16;
                v.pan_target[0] = u32(s32(v.panning[0]) + mstime * 0x10000);
                v.pan_delta[0] = width != 0 ? (mstime << 16) / s32(width) : (mstime << 16);
                break;
            }
            case 0x0F: // Envelope
                tvol_envelope(v, s, s32(v.volume));
                break;
            case 0x10: // StartSample
                mcmd_start_sample(v, s);
                break;
            case 0x11: // StopSample
                hw_break(i);
                break;
            case 0x12: // KeyOff
                v.cflags |= cf_keyoff_cmd;
                synth_key_state_update(v);
                break;
            case 0x13: // SplitRnd
                if (u8(snd_rand()) >= u8(s.p0 >> 8)) {
                    jump_to(v, u16(s.p0 >> 16), u16(s.p1));
                }
                break;
            case 0x14: // FadeIn
                tvol_envelope(v, s, 0);
                break;
            case 0x17: { // RndNote
                u8 k1, k2;
                if (!u8(s.p1 >> 8)) {
                    k1 = u8(s.p0 >> 8);
                    k2 = u8(s.p0 >> 24);
                    if (k1 > k2) {
                        std::swap(k1, k2);
                    }
                }
                else {
                    k1 = u8(std::clamp<s32>(s32(v.cur_note) - u8(s.p0 >> 8), 0, 127));
                    k2 = u8(std::clamp<s32>(s32(v.cur_note) + u8(s.p0 >> 24), 0, 127));
                }
                u8 detune = u8(s.p1) ? u8(s8((snd_rand() % 201) - 100)) : u8(s.p0 >> 16);
                v.cur_note = u8(k1 + (snd_rand() % ((k2 - k1) + 1))) & 0x7F;
                v.cur_detune = s8(detune);
                mcmd_set_key_common(v);
                break; // SetKey's wait has a zero time here
            }
            case 0x18: { // AddNote
                MStep t = s;
                if (u8(s.p0 >> 24) == 0) {
                    v.cur_note = u16(v.cur_note + s8(u8(s.p0 >> 8)));
                }
                else {
                    v.cur_note = u16(v.org_note + s8(u8(s.p0 >> 8)));
                }
                v.cur_note = u16(std::clamp<s32>(s16(v.cur_note), 0, 0x7F));
                v.cur_detune = s8(s.p0 >> 16);
                mcmd_set_key_common(v);
                t.p0 = 4;
                ex = mcmd_wait(v, t);
                break;
            }
            case 0x19: { // SetNote
                MStep t = s;
                v.cur_note = u8(s.p0 >> 8) & 0x7F;
                v.cur_detune = s8(s.p0 >> 16);
                mcmd_set_key_common(v);
                t.p0 = 4;
                ex = mcmd_wait(v, t);
                break;
            }
            case 0x1A: { // LastNote
                MStep t = s;
                v.cur_note = u16(std::clamp<s32>(s32(v.last_note) + s8(u8(s.p0 >> 8)), 0, 0x7F));
                v.cur_detune = s8(s.p0 >> 16);
                inp_set_midi_last_note(v.midi, v.midi_set, u8(v.cur_note));
                t.p0 = 4;
                ex = mcmd_wait(v, t);
                break;
            }
            case 0x1C: { // Vibrato
                if (u8(s.p0 >> 24) & 3) {
                    v.cflags |= cf_vibrato_mod;
                }
                else {
                    v.cflags &= ~cf_vibrato_mod;
                }
                u32 time = u16(s.p1 >> 16);
                time = (u8(s.p1 >> 8) & 1) ? time * 256 : convert_ticks(time);
                if (time) {
                    v.cflags |= cf_vibrato;
                    v.vib_period = time;
                    s8 kr = s8(s.p0 >> 8), cr = s8(s.p0 >> 16);
                    if (kr < 0) {
                        v.vib_cent_range = cr < 0 ? -cr : cr;
                        v.vib_key_range = -kr;
                        v.vib_cur_time = v.vib_period / 2;
                    }
                    else {
                        if (cr < 0) {
                            if (kr == 0) {
                                v.vib_cent_range = -cr;
                                v.vib_cur_time = v.vib_period / 2;
                            }
                            else {
                                --kr;
                                v.vib_cent_range = 100 - cr;
                                v.vib_cur_time = 0;
                            }
                        }
                        else {
                            v.vib_cent_range = cr;
                            v.vib_cur_time = 0;
                        }
                        v.vib_key_range = kr;
                    }
                }
                else {
                    v.cflags &= ~cf_vibrato;
                }
                break;
            }
            case 0x1D: // PitchSweep1
            case 0x1E: { // PitchSweep2
                int num = (s.p0 & 0x7F) == 0x1D ? 0 : 1;
                v.sweep_off[num] = 0;
                v.sweep_num[num] = u8(s.p0 >> 8);
                v.sweep_cnt[num] = s32(v.sweep_num[num]) << 16;
                s32 delta = s16(s.p0 >> 16);
                s32 p = s32((std::abs(delta) * 4096.f) / mix_frq);
                v.sweep_add[num] = (delta >= 0 ? p : -p) * 65536;
                MStep t = s;
                t.p0 = 0;
                ex = mcmd_wait(v, t);
                break;
            }
            case 0x1F: // SetPitch
                v.play_frq = s32((s.p0 >> 8) | u8(s.p1));
                if (v.sinfo != no_id) {
                    do_set_pitch(v);
                }
                break;
            case 0x22: // Mod2Vibrange
                v.vib_mod_add_scale = s8(s.p0 >> 8) << 8;
                if (v.vib_mod_add_scale >= 0) {
                    v.vib_mod_add_scale += (s32(s8(s.p0 >> 16)) << 8) / 100;
                }
                else {
                    v.vib_mod_add_scale -= (s32(s8(s.p0 >> 16)) << 8) / 100;
                }
                break;
            case 0x24: // Return
                if (v.cs_num != 0) {
                    v.addr = v.call_stack[v.cs_index].addr;
                    v.cur = v.call_stack[v.cs_index].cur;
                    v.cs_index = (v.cs_index - 1) & 3;
                    --v.cs_num;
                }
                break;
            case 0x25: // GoSub
                if (const std::vector<MStep>* m = data_get_macro(u16(s.p0 >> 16))) {
                    v.cs_index = (v.cs_index + 1) & 3;
                    v.call_stack[v.cs_index] = { v.addr, v.cur };
                    if (++v.cs_num > 4) {
                        v.cs_num = 4;
                    }
                    v.addr = m;
                    v.cur = u16(s.p1);
                }
                else {
                    ex = mcmd_end(v);
                }
                break;
            case 0x30: { // AddAgeCount
                s16 step = s16(s.p0 >> 16);
                s32 age = s32(v.age >> 15) + step;
                v.age = age < 0 ? 0 : age > 0xFFFF ? 0x7FFF8000u : u32(age) * 0x8000;
                break;
            }
            case 0x31: // SetAgeCount
                v.age = u32(u16(s.p0 >> 16)) << 15;
                break;
            case 0x33: // PitchWheelR
                v.pb_lower = u8(s.p0 >> 16);
                v.pb_upper = u8(s.p0 >> 8);
                break;
            case 0x36: // SetPriority
                voice_set_priority(v, u8(s.p0 >> 8));
                break;
            case 0x37: { // AddPriority
                s16 prio = s16(v.prio + s16(s.p0 >> 16));
                voice_set_priority(v, u8(std::clamp<s16>(prio, 0, 0xFF)));
                break;
            }
            case 0x38: // AgeCntSpeed
                v.age_speed = s.p1 != 0 ? (v.age >> 8) / s.p1 : 0;
                break;
            case 0x39: { // AgeCntVel
                u32 age = ((u8(v.volume >> 16) * u16(s.p1)) >> 7) + u16(s.p0 >> 16);
                v.age = age > 60000 ? 0x75300000u : age * 0x8000;
                break;
            }
            case 0x40: // VolSelect
                select_source(v, v.inp_vol, s, cf_vol_select);
                break;
            case 0x41: // PanSelect
                select_source(v, v.inp_pan, s, cf_pan_select);
                break;
            case 0x42: // PitchWheelSelect
                select_source(v, v.inp_pb, s, cf_pb_select);
                break;
            case 0x43: // ModWheelSelect
                select_source(v, v.inp_mod, s, cf_mod_select);
                break;
            case 0x47: // SpanSelect
                select_source(v, v.inp_span, s, cf_span_select);
                break;
            case 0x48: // DopplerSelect
                select_source(v, v.inp_doppler, s, cf_doppler_select);
                break;
            default:
                break;
            }
            if (ex) {
                return;
            }
        }
    }

    void mac_handle(u32 delta_time) {
        // Wake voices whose wait has passed (the MusyX time queue).
        for (SVoice& v : eng->sv) {
            if (v.vid != no_id && v.mac_state == MacState::yielded && v.wait != 0 && v.wait != ~u64(0) &&
                v.wait <= eng->mac_real_time) {
                u64 w = v.wait;
                mac_make_active(v);
                v.wait_time = w;
            }
        }
        for (int i = 0; i < voice_num; i++) {
            SVoice& v = eng->sv[i];
            if (v.vid != no_id && v.mac_state == MacState::runnable) {
                mac_handle_active(i);
            }
        }
        eng->mac_real_time += delta_time;
    }

    u32 mac_start(u16 macid, u8 priority, u8 max_voices, u16 alloc_id, u8 key, u8 vol, u8 panning, u8 midi,
                  u8 midi_set, u16 step, u8 track, u8 vgroup) {
        const std::vector<MStep>* addr = data_get_macro(macid);
        if (addr == nullptr) {
            return no_id;
        }
        bool fx = (key & 0x80) != 0;
        if (!fx && eng->seq.active && midi < 16 && eng->seq.midi_priority[midi] != 0xFFFF) {
            priority = u8(eng->seq.midi_priority[midi]);
        }
        int i = voice_allocate(priority, max_voices, alloc_id, fx);
        if (i < 0) {
            return no_id;
        }
        SVoice& v = eng->sv[i];
        bool was_counted = v.counted;
        mac_make_inactive(v, MacState::stopped);
        v.cflags = (v.cflags & cf_started) | cf_new;
        if (hw_is_active(i)) {
            v.cflags |= cf_break_on_start;
        }
        v.wait = 0;
        v.fx = fx;
        if (fx) {
            key &= 0x7F;
            inp_reset_midi_ctrl(u8(i), fx_set, true);
            inp_reset_channel_defaults(u8(i), fx_set);
            v.setup_midi = u8(i);
            v.setup_set = fx_set;
        }
        else {
            v.setup_midi = midi;
            v.setup_set = midi_set;
        }
        v.macro_id = macid;
        v.alloc_id = alloc_id;
        v.age = 0x75300000;
        v.age_speed = 0x400;
        v.addr = addr;
        v.cur = step;
        v.org_note = key;
        v.cur_note = key;
        v.cur_detune = 0;
        v.setup_vol = vol;
        v.setup_pan = panning;
        v.setup_track = track;
        v.setup_vgroup = vgroup;
        v.cs_num = 0;
        v.cs_index = 0;
        v.vid = (eng->gen << 8) | u32(i);
        eng->gen = (eng->gen + 1) & 0x7FFFFF;
        if (eng->gen == 0) {
            eng->gen = 1;
        }
        v.counted = was_counted;
        v.job_low = v.job_zero = v.job_event = -1;
        voice_set_priority(v, priority);
        mac_make_active(v);
        v.mac_state = MacState::runnable;
        return v.vid;
    }

    u32 synth_start_sound(u16 id, u8 prio, u8 max, u8 key, u8 vol, u8 panning, u8 midi, u8 midi_set, u8 track,
                          u8 vgroup, s16 prio_offset) {
        s32 p = std::clamp<s32>(prio + prio_offset, 0, 0xFF);
        if ((id & 0xC000) != 0) {
            return no_id; // keymaps and layers: Rush 2049 has none
        }
        return mac_start(id, u8(p), max, id, key, vol, panning, midi, midi_set, 0, track, vgroup);
    }

    SVoice* voice_by_vid(u32 vid) {
        if (vid == no_id) {
            return nullptr;
        }
        SVoice& v = eng->sv[(vid & 0xFF) % voice_num];
        return v.vid == vid ? &v : nullptr;
    }

    void synth_send_key_off(u32 vid) {
        if (SVoice* v = voice_by_vid(vid)) {
            mac_set_external_keyoff(*v);
        }
    }

    void voice_kill(int i) {
        if (hw_is_active(i)) {
            hw_break(i);
        }
        voice_free(i);
    }

    void voice_kill_sound(u32 vid) {
        if (SVoice* v = voice_by_vid(vid)) {
            voice_kill(voice_index(*v));
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Faders (synthVolume; N64 boot func_8001B9F8: 16.16 volume stepped by (target - volume) / time each ms)

    void seq_stop();

    void synth_volume(u8 volume, u16 time_ms, int group, int seq_mode) {
        Fader& f = eng->faders[group & 31];
        f.target = s32(volume) << 16;
        f.seq_mode = seq_mode;
        if (time_ms == 0) {
            time_ms = 1;
        }
        f.delta = (f.target - f.volume) / s32(time_ms);
        f.active = true;
        if (f.delta == 0) {
            f.volume = f.target;
        }
    }

    void handle_faders() {
        for (int g = 0; g < 32; g++) {
            Fader& f = eng->faders[g];
            if (!f.active) {
                continue;
            }
            f.volume += f.delta;
            if ((f.delta >= 0 && f.volume >= f.target) || (f.delta < 0 && f.volume <= f.target)) {
                f.volume = f.target;
                f.active = false;
                if (f.seq_mode == 1) {
                    f.seq_mode = 0;
                    if (eng->seq.active && eng->seq.def_vgroup == g) {
                        seq_stop();
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Sequencer (seq.c)

    u32 arr32(const Seq& s, u32 off) {
        return off + 4 <= s.arr_size ? be32(s.arr + off) : 0xFFFFFFFF;
    }
    u16 arr16(const Seq& s, u32 off) {
        return off + 2 <= s.arr_size ? be16(s.arr + off) : 0xFFFF;
    }
    u8 arr8(const Seq& s, u32 off) {
        return off < s.arr_size ? s.arr[off] : 0xFF;
    }

    // Returns the offset after the value, or 0 at the end marker.
    u32 get_stream_value(const Seq& s, u32 p, u16& delta_time, s16& delta_data) {
        u8 b1 = arr8(s, p), b2 = arr8(s, p + 1);
        if (b1 == 0x80 && b2 == 0) {
            return 0;
        }
        if (b1 & 0x80) {
            delta_time = u16(((b1 & 0x7F) << 8) | b2);
            p += 2;
        }
        else {
            delta_time = b1;
            p += 1;
        }
        b1 = arr8(s, p);
        b2 = arr8(s, p + 1);
        if (b1 & 0x80) {
            s16 v = s16(((b1 & 0x7F) << 8) | b2);
            v = s16(v | ((v & 0x4000) << 1));
            delta_data = v;
            p += 2;
        }
        else {
            b1 = u8(b1 | ((b1 & 0x40) << 1));
            delta_data = s8(b1);
            p += 1;
        }
        return p;
    }

    void init_stream(const Seq& s, Stream& st, u32 offset) {
        st.next_time = 0x7FFFFFFF;
        st.next = 0;
        if (offset != 0) {
            u16 delta;
            st.next = get_stream_value(s, offset, delta, st.next_delta);
            if (st.next != 0) {
                st.next_time = delta;
            }
        }
    }

    u16 handle_stream(const Seq& s, Stream& st) {
        st.value = u16(st.value + st.next_delta);
        if (st.next != 0) {
            u16 delta;
            st.next = get_stream_value(s, st.next, delta, st.next_delta);
            if (st.next != 0) {
                st.next_time += delta;
            }
            else {
                st.next_time = 0x7FFFFFFF;
            }
        }
        else {
            st.next_time = 0x7FFFFFFF;
        }
        return st.value;
    }

    bool generate_next_track_event(Seq& s, int t) {
        SeqTrack& tr = s.track[t];
        SeqPattern& pa = s.pattern[t];
        SeqEvent& ev = s.event[t];
        if (tr.addr == 0) {
            return false;
        }
        for (;;) {
            if (pa.addr == 0) {
                u16 pat = arr16(s, tr.addr + 8);
                if (pat == 0xFFFF) {
                    tr.addr = 0;
                    return false;
                }
                if (pat == 0xFFFE) {
                    if (s.loop_disable) {
                        tr.addr = 0;
                        return false;
                    }
                    ev.type = 3;
                    ev.time = arr32(s, tr.addr);
                    tr.addr = tr.base + 12 * u32(arr16(s, tr.addr + 10));
                    return true;
                }
                ev.type = 4;
                ev.time = arr32(s, tr.addr);
                ev.addr = tr.addr;
                tr.addr += 12;
                return true;
            }
            u32 pitch_time = pa.pb.next_time;
            u32 mod_time = pa.mod.next_time;
            for (;;) {
                u32 pattern_time = arr16(s, pa.addr) + pa.l_time;
                if (pattern_time >= pitch_time || pattern_time >= mod_time) {
                    if (pattern_time >= pitch_time && pitch_time < mod_time) {
                        ev.time = pitch_time + pa.base_time;
                        ev.type = 2;
                    }
                    else {
                        ev.time = mod_time + pa.base_time;
                        ev.type = 1;
                    }
                    return true;
                }
                u8 key = arr8(s, pa.addr + 2), vel = arr8(s, pa.addr + 3);
                if (key == 0xFF && vel == 0xFF) {
                    pa.addr = 0;
                    break;
                }
                ev.addr = pa.addr;
                pa.l_time = pattern_time;
                if (key & 0x80) {
                    pa.addr += 4;
                }
                else if ((key | vel) == 0) {
                    pa.addr += 4;
                    continue;
                }
                else {
                    pa.addr += 6;
                }
                ev.type = 0;
                ev.time = pattern_time + pa.base_time;
                return true;
            }
        }
    }

    void insert_event(Seq& s, int t) {
        u32 time = s.event[t].time;
        auto it = std::find_if(s.ev_list.begin(), s.ev_list.end(), [&](u8 o) { return s.event[o].time > time; });
        s.ev_list.insert(it, u8(t));
    }

    void do_prg_change(Seq& s, u8 prg, u8 midi) {
        s.midi_priority[midi & 15] = 0xFFFF;
        const Page& page = midi != 9 ? s.group->norm[prg & 0x7F] : s.group->drum[prg & 0x7F];
        if (page.macro == 0xFFFF) {
            return;
        }
        s.prg[midi & 15] = { page.macro, page.prio, page.max_voices };
    }

    void key_off_notes(Seq& s) {
        for (SeqNote& n : s.notes) {
            synth_send_key_off(n.vid);
        }
        s.notes.clear();
    }

    bool handle_event(Seq& s, int t, bool& loop_flag) {
        SeqEvent& ev = s.event[t];
        SeqPattern& pa = s.pattern[t];
        switch (ev.type) {
        case 4: {
            u32 entry = ev.addr;
            u32 ptab = arr32(s, 4);
            u32 pptr = arr32(s, ptab + 4 * arr16(s, entry + 8));
            if (pptr == 0xFFFFFFFF || pptr + 12 > s.arr_size) {
                pa.addr = 0;
                break;
            }
            pa.addr = pptr + 12;
            pa.l_time = 0;
            pa.base_time = ev.time;
            pa.entry = entry;
            init_stream(s, pa.pb, arr32(s, pptr + 4));
            pa.pb.value = 0x2000;
            init_stream(s, pa.mod, arr32(s, pptr + 8));
            pa.mod.value = 0;
            pa.midi = arr8(s, arr32(s, 8) + u32(t)) & 15;
            u8 prg = arr8(s, entry + 4);
            if (prg != 0xFF) {
                do_prg_change(s, prg, pa.midi);
            }
            u8 vol = arr8(s, entry + 5);
            if (vol != 0xFF) {
                inp_set_midi_ctrl(7, pa.midi, s.set, vol);
            }
            break;
        }
        case 0: {
            s32 key = arr8(s, ev.addr + 2);
            s32 velocity = arr8(s, ev.addr + 3);
            u8 midi = pa.midi;
            if (key & 0x80) {
                switch (velocity) {
                case 0:
                    do_prg_change(s, u8(key & 0x7F), midi);
                    break;
                case 1:
                    inp_set_midi_ctrl(0x82, midi, s.set, u8(key & 0x7F));
                    break;
                default:
                    if ((velocity & 0x80) != 0x80) {
                        break;
                    }
                    switch (velocity & 0x7F) {
                    case 0x68:
                        break; // synchronised crossfade: unused
                    case 0x69:
                        s.midi_priority[midi] = u16(key & 0x7F);
                        break;
                    case 0x6A:
                        s.midi_priority[midi] = u16((key & 0x7F) + 0x80);
                        break;
                    case 0x79:
                        inp_reset_midi_ctrl(midi, s.set, false);
                        break;
                    case 0x7B:
                        key_off_notes(s);
                        break;
                    default:
                        inp_set_midi_ctrl(u8(velocity & 0x7F), midi, s.set, u8(key & 0x7F));
                        break;
                    }
                }
                break;
            }
            u16 mac = s.prg[midi].macro;
            if (mac != 0xFFFF) {
                key = std::clamp<s32>(key + s8(arr8(s, pa.entry + 10)), 0, 0x7F);
                velocity = std::clamp<s32>(velocity + s8(arr8(s, pa.entry + 11)), 0, 0x7F);
                s32 end_time = s32(ev.time + arr16(s, ev.addr + 4));
                bool fading = eng->faders[s.def_vgroup].active && eng->faders[s.def_vgroup].delta < 0;
                u32 vid = synth_start_sound(mac, s.prg[midi].prio, s.prg[midi].max_voices, u8(key), u8(velocity), 64,
                                            midi, s.set, u8(t), s.def_vgroup, fading ? -1 : 0);
                if (vid != no_id) {
                    s.notes.push_back({ vid, end_time, s.time_index });
                    eng->notes_started++;
                    eng->channel_notes[midi]++;
                }
                else {
                    eng->notes_dropped++;
                }
            }
            break;
        }
        case 2:
            inp_set_midi_ctrl14(128, pa.midi, s.set, handle_stream(s, pa.pb));
            break;
        case 1:
            inp_set_midi_ctrl14(1, pa.midi, s.set, handle_stream(s, pa.mod));
            break;
        case 3:
            loop_flag = true;
            return false;
        }
        return generate_next_track_event(s, t);
    }

    void init_track_events(Seq& s) {
        for (int t = 0; t < 64; t++) {
            if (generate_next_track_event(s, t)) {
                insert_event(s, t);
            }
        }
    }

    void set_tick_delta(Seq& s, u32 delta_time) {
        float tick_delta = float(s.bpm) * float(delta_time) * (1.f / 40960000.f);
        tick_delta *= float(s.speed) * (1.f / 256.f);
        s.tick_delta[s.time_index].low = u32(std::fmod(tick_delta * 65536.f, 65536.f));
        s.tick_delta[s.time_index].high = s32(std::floor(tick_delta));
    }

    void handle_master_track(Seq& s) {
        if (s.m_track == 0) {
            return;
        }
        for (;;) {
            u32 time = arr32(s, s.m_addr);
            if (time == 0xFFFFFFFF || s32(time) > s.time[s.time_index].high) {
                break;
            }
            u32 bpm = arr32(s, s.m_addr + 4);
            u32 info = arr32(s, 0x10);
            s.bpm = (info & 0x40000000) ? bpm : (bpm << 10);
            s.m_addr += 8;
        }
    }

    bool handle_track_events(Seq& s, u32 delta_time) {
        bool loop_flag = false;
        for (;;) {
            u32 next = s.ev_list.empty() ? 0 : s.event[s.ev_list.front()].time;
            if (!(s32(next) <= s.time[s.time_index].high)) {
                break;
            }
            if (s.ev_list.empty()) {
                if (!loop_flag) {
                    return false;
                }
                loop_flag = false;
                s.time_index ^= 1;
                s.time[s.time_index].high = s32(arr32(s, 0x14));
                s.time[s.time_index].low = s.time[s.time_index ^ 1].low;
                if (s.m_track) {
                    s.m_addr = s.m_track;
                    handle_master_track(s);
                    set_tick_delta(s, delta_time);
                }
                s.loop_cnt++;
                eng->loops++;
                init_track_events(s);
                continue;
            }
            int t = s.ev_list.front();
            s.ev_list.erase(s.ev_list.begin());
            if (handle_event(s, t, loop_flag)) {
                insert_event(s, t);
            }
        }
        return true;
    }

    bool handle_notes(Seq& s) {
        for (size_t k = 0; k < s.notes.size();) {
            SeqNote& n = s.notes[k];
            if (n.end_time <= s.time[n.time_index].high) {
                synth_send_key_off(n.vid);
                s.notes.erase(s.notes.begin() + k);
            }
            else {
                k++;
            }
        }
        return !s.notes.empty();
    }

    void seq_kill_notes(Seq& s) {
        for (SeqNote& n : s.notes) {
            voice_kill_sound(n.vid);
        }
        s.notes.clear();
        // Released notes are no longer tracked here; kill every voice of the song.
        for (int i = 0; i < voice_num; i++) {
            SVoice& v = eng->sv[i];
            if (v.vid != no_id && !v.fx && v.midi_set == s.set) {
                voice_kill(i);
            }
            else if (eng->hw[i].active && !eng->hw[i].fx && (v.vid == no_id || v.vid != eng->hw[i].owner)) {
                hw_break(i);
            }
        }
    }

    void seq_stop() {
        Seq& s = eng->seq;
        if (!s.active) {
            return;
        }
        seq_kill_notes(s);
        s.active = false;
        s.song = -1;
        s.ev_list.clear();
    }

    void seq_handle(u32 delta_time) {
        Seq& s = eng->seq;
        if (!s.active) {
            return;
        }
        handle_master_track(s);
        set_tick_delta(s, delta_time);
        bool events_active = handle_track_events(s, delta_time);
        bool notes_active = handle_notes(s);
        for (int i = 0; i < 2; i++) {
            u32 x = s.time[i].low + s.tick_delta[i].low;
            s.time[i].low = x & 0xFFFF;
            x >>= 16;
            s.time[i].high += s32(x) + s.tick_delta[i].high;
        }
        if (!events_active && !notes_active) {
            s.active = false;
            s.song = -1;
            s.ev_list.clear();
        }
    }

    void seq_start(int song) {
        Seq& s = eng->seq;
        const std::vector<u8>& arr = eng->data.songs[song];
        const SongGroup& g = eng->data.groups[song];
        s = Seq{};
        s.active = true;
        s.song = song;
        s.arr = arr.data();
        s.arr_size = arr.size();
        s.group = &g;
        s.set = 0;
        s.def_vgroup = 23; // seqId + 23
        for (u16& p : s.midi_priority) {
            p = 0xFFFF;
        }
        Fader& f = eng->faders[s.def_vgroup];
        f.volume = f.target = 0x7F0000;
        f.active = false;
        f.seq_mode = 0;

        u32 info = arr32(s, 0x10);
        u32 bpm = info & 0x0FFFFFFF;
        if (!(info & 0x40000000)) {
            bpm <<= 10;
        }
        s.bpm = bpm;
        u32 mt = arr32(s, 0xC);
        s.m_track = (mt != 0 && mt < s.arr_size) ? mt : 0;
        s.m_addr = s.m_track;
        u32 ttab = arr32(s, 0);
        for (int t = 0; t < 64; t++) {
            eng->track_volume[t] = 127;
            u32 off = arr32(s, ttab + 4 * t);
            s.track[t].base = s.track[t].addr = (off != 0 && off != 0xFFFFFFFF && off < s.arr_size) ? off : 0;
        }
        for (int c = 0; c < 16; c++) {
            inp_reset_midi_ctrl(u8(c), s.set, true);
            inp_reset_channel_defaults(u8(c), s.set);
        }
        if (g.has_setup) {
            for (int c = 0; c < 16; c++) {
                do_prg_change(s, g.setup[c].program, u8(c));
                inp_set_midi_ctrl(7, u8(c), s.set, g.setup[c].volume);
                inp_set_midi_ctrl(10, u8(c), s.set, g.setup[c].pan);
                inp_set_midi_ctrl(91, u8(c), s.set, g.setup[c].reverb);
                inp_set_midi_ctrl(93, u8(c), s.set, g.setup[c].chorus);
            }
        }
        for (u16& p : s.midi_priority) {
            p = 0xFFFF;
        }
        init_track_events(s);
        eng->notes_started = eng->notes_dropped = eng->loops = 0;
        std::memset(eng->channel_notes, 0, sizeof(eng->channel_notes));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Engine step and mixing

    void engine_step() {
        eng->time_offset = u32(eng->step_count % 5);
        seq_handle(256);
        mac_handle(256);
        handle_voices();
        handle_faders(); // the N64 build steps its faders every millisecond
        eng->synth_real_time += 256;
        eng->job_tick++;
        hw_prepare_ms();
        eng->step_count++;
    }

    float sample_at(const Sample& s, s64 i) {
        if (i < 0) {
            return float(s.pcm[0]);
        }
        if (s.loop_length != 0) {
            s64 end = s64(s.loop_start) + s.loop_length;
            if (i >= end) {
                i = s.loop_start + (i - s.loop_start) % s.loop_length;
            }
        }
        else if (i >= s64(s.length)) {
            return 0.f;
        }
        return float(s.pcm[size_t(i)]);
    }

    // Renders n samples of voice h into out with left/right gains; returns false if the sample ended.
    bool render_voice(HwVoice& h, float* out, u32 n, u32 ms_pos, u32 ms_len, float bus_gain, bool ramp_volume,
                      float tail_l = 0.f, float tail_r = 0.f, u32 tail_left = 0, u32 tail_total = 0) {
        const Sample& s = *h.smp;
        double step = double(h.pitch) / 4096.0 * double(mix_frq) / double(eng->rate);
        u32 ramp_len = std::max<u32>(1, eng->rate / 200);
        double loop_end = s.loop_length != 0 ? double(s.loop_start) + s.loop_length : double(s.length);
        for (u32 k = 0; k < n; k++) {
            float gl, gr;
            if (ramp_volume) {
                float t = h.ramp_pos >= ramp_len ? 1.f : float(h.ramp_pos) / ramp_len;
                if (h.ramp_pos < ramp_len) {
                    h.ramp_pos++;
                }
                float a = h.adsr_from + (h.adsr_to - h.adsr_from) * (float(ms_pos + k) / float(ms_len));
                gl = (h.ramp_from[0] + (h.ramp_to[0] - h.ramp_from[0]) * t) * a;
                gr = (h.ramp_from[1] + (h.ramp_to[1] - h.ramp_from[1]) * t) * a;
            }
            else {
                float f = float(tail_left - k) / float(tail_total);
                gl = tail_l * f;
                gr = tail_r * f;
            }
            s64 idx = s64(std::floor(h.pos));
            float t = float(h.pos - double(idx));
            float p0 = sample_at(s, idx - 1), p1 = sample_at(s, idx);
            float p2 = sample_at(s, idx + 1), p3 = sample_at(s, idx + 2);
            float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                                                      t * (3.0f * (p1 - p2) + p3 - p0)));
            v *= (output_gain / 32768.0f) * bus_gain;
            out[k * 2 + 0] += v * gl;
            out[k * 2 + 1] += v * gr;
            h.pos += step;
            if (h.pos >= loop_end) {
                if (s.loop_length != 0) {
                    while (h.pos >= loop_end) {
                        h.pos -= s.loop_length;
                    }
                }
                else {
                    return false;
                }
            }
        }
        return true;
    }

    void render(float* out, u32 n, float music_gain, float sfx_gain) {
        for (int i = 0; i < voice_num; i++) {
            HwVoice& h = eng->hw[i];
            if (!h.active || h.starting || h.smp == nullptr || h.smp->pcm.empty()) {
                continue;
            }
            float g = h.fx ? sfx_gain : music_gain;
            if (!render_voice(h, out, n, eng->ms_pos, eng->ms_len, g, true)) {
                hw_deactivate(i, true, false);
            }
        }
        for (size_t k = 0; k < eng->tails.size();) {
            Tail& t = eng->tails[k];
            u32 m = std::min(n, t.left);
            float g = t.v.fx ? sfx_gain : music_gain;
            bool alive = render_voice(t.v, out, m, 0, 1, g, false, t.gain_l, t.gain_r, t.left, t.total);
            t.left -= m;
            if (!alive || t.left == 0) {
                eng->tails.erase(eng->tails.begin() + k);
            }
            else {
                k++;
            }
        }
    }

    void reset_engine() {
        Data data = std::move(eng->data);
        *eng = Engine{};
        eng->data = std::move(data);
        for (int i = 0; i < voice_num; i++) {
            eng->free_list.push_back(i);
            set_cold_defaults(eng->fx_ctrl[i]);
            eng->fx_pb_range[i] = 2;
        }
        for (int s = 0; s < 8; s++) {
            for (int c = 0; c < 16; c++) {
                set_cold_defaults(eng->midi_ctrl[s][c]);
                eng->pb_range[s][c] = 2;
                eng->midi_last_note[s][c] = 0xFF;
            }
        }
        for (u8& n : eng->fx_last_note) {
            n = 0xFF;
        }
        for (u8& t : eng->track_volume) {
            t = 127;
        }
        // Rush 2049: sndVolume(127, 0, all groups), then sndMasterVolume(114) for music and sfx (default options).
        eng->faders[master_music_fader].volume = eng->faders[master_music_fader].target = default_master << 16;
        eng->faders[master_sfx_fader].volume = eng->faders[master_sfx_fader].target = default_master << 16;
        eng->rand_state = 0x12345678;
    }

    void set_fx_controllers(SVoice& v, float volume, float pan, float pitch, float surround) {
        u8 ch = u8(voice_index(v));
        auto byte = [](float f) { return u8(std::clamp(f, 0.f, 1.f) * 127.f); };
        inp_set_midi_ctrl(7, ch, fx_set, byte(volume));
        inp_set_midi_ctrl(10, ch, fx_set, byte((std::clamp(pan, -1.f, 1.f) + 1.f) * 0.5f));
        inp_set_midi_ctrl(131, ch, fx_set, byte((std::clamp(surround, -1.f, 1.f) + 1.f) * 0.5f));
        float d = pitch * 8192.f - 1.f;
        u16 doppler = u16(std::clamp(d, 0.f, 16383.f));
        inp_set_midi_ctrl14(132, ch, fx_set, doppler);
    }
}

namespace rush2::audio2049 {
    bool load(const std::vector<uint8_t>& rom) {
        dc::unload();
        auto fresh = std::make_unique<Engine>();
        if (!parse(rom, fresh->data)) {
            return false;
        }
        std::lock_guard lock{ engine_mutex };
        delete eng;
        eng = fresh.release();
        reset_engine();
        return true;
    }

    bool load(std::shared_ptr<const rush2::rom2049::Source> source) {
        if (source == nullptr) {
            return false;
        }
        if (const std::vector<uint8_t>* rom = source->n64_rom()) {
            return load(*rom);
        }
        if (!dc::load(source)) {
            return false;
        }
        std::lock_guard lock{ engine_mutex };
        delete eng;
        eng = nullptr;
        return true;
    }

    void set_track(int track_id) {
        dc::set_track(track_id);
    }

    bool sfx_samples(int id, std::vector<int16_t>& pcm, uint32_t& rate) {
        return dc::sfx_samples(id, pcm, rate);
    }

    bool loaded() {
        if (dc::active()) {
            return true;
        }
        std::lock_guard lock{ engine_mutex };
        return eng != nullptr && eng->data.ok;
    }

    int track_song(int track) {
        if (dc::active()) {
            return dc::track_song(track);
        }
        return track >= 0 && track < 19 ? track_songs[track] : -1;
    }

    void play_song(int song) {
        if (dc::active()) {
            dc::play_song(song);
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr) {
            return;
        }
        seq_stop();
        if (song < 0 || song >= song_count) {
            return;
        }
        seq_start(song);
    }

    void stop_song(float fade_seconds) {
        if (dc::active()) {
            dc::stop_song(fade_seconds);
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || !eng->seq.active) {
            return;
        }
        if (fade_seconds <= 0.f) {
            seq_stop();
            return;
        }
        u32 ms = u32(std::min(fade_seconds * 1000.f, 65535.f));
        synth_volume(0, u16(ms), eng->seq.def_vgroup, 1);
    }

    bool song_playing() {
        if (dc::active()) {
            return dc::song_playing();
        }
        std::lock_guard lock{ engine_mutex };
        return eng != nullptr && eng->seq.active;
    }

    int current_song() {
        if (dc::active()) {
            return dc::current_song();
        }
        std::lock_guard lock{ engine_mutex };
        return eng != nullptr && eng->seq.active ? eng->seq.song : -1;
    }

    int sfx_start(int id, float volume, float pan, float pitch, float surround) {
        if (dc::active()) {
            return dc::sfx_start(id, volume, pan, pitch, surround);
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || id < 0 || id >= int(eng->data.fx.size()) || !eng->data.fx[id].valid) {
            return -1;
        }
        const Fx& fx = eng->data.fx[id];
        u32 vid = synth_start_sound(fx.macro, fx.prio, fx.max_voices, u8(fx.key | 0x80), fx.volume, fx.pan, 0xFF,
                                    0xFF, 0xFF, fx.vgroup, 0);
        if (vid == no_id) {
            return -1;
        }
        SVoice& v = eng->sv[vid & 0xFF];
        set_fx_controllers(v, volume, pan, pitch, surround);
        eng->last_sfx_vid = vid;
        return int(vid);
    }

    void sfx_update(int handle, float volume, float pan, float pitch, float surround) {
        if (dc::active()) {
            dc::sfx_update(handle, volume, pan, pitch, surround);
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || handle < 0) {
            return;
        }
        if (SVoice* v = voice_by_vid(u32(handle))) {
            set_fx_controllers(*v, volume, pan, pitch, surround);
        }
    }

    void sfx_stop(int handle) {
        if (dc::active()) {
            dc::sfx_stop(handle);
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || handle < 0) {
            return;
        }
        synth_send_key_off(u32(handle));
    }

    bool sfx_active(int handle) {
        if (dc::active()) {
            return dc::sfx_active(handle);
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || handle < 0) {
            return false;
        }
        u32 vid = u32(handle);
        int i = int(vid & 0xFF) % voice_num;
        return eng->sv[i].vid == vid || (eng->hw[i].active && eng->hw[i].owner == vid);
    }

    void sfx_stop_all() {
        if (dc::active()) {
            dc::sfx_stop_all();
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr) {
            return;
        }
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].vid != no_id && eng->sv[i].fx) {
                voice_kill(i);
            }
            else if (eng->hw[i].active && eng->hw[i].fx) {
                hw_break(i);
            }
        }
    }

    void mix(float* out, size_t frames, uint32_t sample_rate, float music_gain, float sfx_gain) {
        if (dc::active()) {
            dc::mix(out, frames, sample_rate, music_gain, sfx_gain);
            return;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr || !eng->data.ok || sample_rate == 0) {
            return;
        }
        if (eng->rate != sample_rate) {
            eng->rate = sample_rate;
            eng->ms_acc = 0;
            eng->ms_left = 0;
        }
        size_t done = 0;
        while (done < frames) {
            if (eng->ms_left == 0) {
                engine_step();
                eng->ms_acc += sample_rate;
                eng->ms_len = eng->ms_acc / 1000;
                eng->ms_acc -= eng->ms_len * 1000;
                eng->ms_left = eng->ms_len;
                eng->ms_pos = 0;
                continue;
            }
            u32 n = u32(std::min<size_t>(eng->ms_left, frames - done));
            render(out + done * 2, n, music_gain, sfx_gain);
            done += n;
            eng->ms_left -= n;
            eng->ms_pos += n;
        }
    }

    EmitterParams emitter_mix(const float emitter[3], const float listener[3], const float back[3], const float up[3],
                              float range, float max_volume, float min_volume) {
        max_volume = std::clamp(max_volume, 0.f, 1.f);
        min_volume = std::clamp(min_volume, 0.f, 1.f);
        float d[3] = { emitter[0] - listener[0], emitter[1] - listener[1], emitter[2] - listener[2] };
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist == 0.f) {
            d[0] = 1.f;
            d[1] = d[2] = 0.f;
        }
        else {
            for (float& c : d) {
                c *= 1.f / dist;
            }
        }
        // right = up x back (func_80098AE4 builds C = B x A with A = listener +0x24, B = listener +0x30).
        float right[3] = { up[1] * back[2] - back[1] * up[2], up[2] * back[0] - up[0] * back[2],
                           up[0] * back[1] - up[1] * back[0] };
        float pan = right[0] * d[0] + right[1] * d[1] + right[2] * d[2];
        float sur = back[0] * d[0] + back[1] * d[1] + back[2] * d[2];
        float g;
        if (range > 0.f) {
            g = max_volume - (max_volume - min_volume) * (dist / range);
            if (g < 0.f) {
                g = 0.f;
            }
        }
        else {
            g = max_volume;
        }
        EmitterParams p;
        p.volume = std::clamp(g, 0.f, 1.f);
        p.pan = std::clamp(pan - pan * g, -1.f, 1.f);
        p.surround = std::clamp(sur - sur * g, -1.f, 1.f);
        if (g == 0.f) {
            p.pan = p.surround = 0.f; // Rush 2049 skips the weighting when the summed volume is zero
        }
        return p;
    }

    Stats stats() {
        Stats st;
        if (dc::active()) {
            dc::stats(st);
            return st;
        }
        std::lock_guard lock{ engine_mutex };
        if (eng == nullptr) {
            return st;
        }
        st.song = eng->seq.active ? eng->seq.song : -1;
        st.song_tick = eng->seq.active ? u32(eng->seq.time[eng->seq.time_index].high) : 0;
        st.song_loops = eng->loops;
        st.notes_started = eng->notes_started;
        st.notes_dropped = eng->notes_dropped;
        for (int i = 0; i < voice_num; i++) {
            if (eng->sv[i].vid != no_id) {
                (eng->sv[i].fx ? st.voices_sfx : st.voices_music)++;
            }
            if (eng->hw[i].active) {
                st.voices_sounding++;
            }
        }
        std::memcpy(st.channel_notes, eng->channel_notes, sizeof(st.channel_notes));
        if (eng->last_sfx_vid != no_id) {
            const HwVoice& h = eng->hw[eng->last_sfx_vid & 0xFF];
            if (h.owner == eng->last_sfx_vid) {
                st.sfx_voice_gain[0] = u32(std::max<s16>(h.vol_l, 0));
                st.sfx_voice_gain[1] = u32(std::max<s16>(h.vol_r, 0));
            }
        }
        return st;
    }
}

