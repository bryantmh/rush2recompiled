// Wing sound, played the way Rush 2049 plays it.
//
// Rush 2049 starts sound 0x3D when a car's wings start sliding out or in and keys it off when the slide finishes
// (func_800924F4); a reversal halfway keeps the same sound going. The sound is a looping wind sample at a fixed
// pitch and volume with no panning or distance effects, one voice per car. Key-off fades it out linearly over
// 20 ms.
//
// Rush 2049 uses its own synth and RSP mixer, not libaudio. Sound 0x3D plays sample 0x120: 9391 samples at
// ROM 0x2E2540 that loop as a whole, in the mixer's ADPCM format. The sample starts with a 0x100-byte codebook
// (8 predictors, order 2, Nintendo VADPCM layout), then 40-byte blocks of 64 samples. Each block is four int16
// anchors (two per half) and two 16-byte halves of 32 samples. A half's first byte holds the predictor (high nibble)
// and shift (low nibble) and its nibbles 2-31 are residuals; its first two samples are its anchors, so each half
// decodes on its own.
//
// The game plays it at pitch 0.75: 1535/4096 sample steps per output sample at Rush 2049's 22050 Hz output, at a
// volume that comes out at 0.1089 (left) and 0.1065 (right) of full scale with the default sound effects volume.
//
// From a Dreamcast disc the sound is the disc's counterpart of 0x3D (its 0x4D: an 11025 Hz PCM16 loop, src/rush2049dc/audio2049_dc.cpp),
// taken once the disc's sound banks have loaded, at the same pitch and gains [I: the disc's wing code isn't traced].

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>

#include "audio2049.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t sample_rom = 0x2E2540;
    constexpr uint32_t sample_length = 9391;
    constexpr uint32_t codebook_size = 0x100;
    constexpr uint32_t block_size = 40;
    constexpr uint32_t block_samples = 64;

    constexpr double source_step_at_22050 = 1535.0 / 4096.0;
    constexpr float gain_left = 3568.0f / 32768.0f;
    constexpr float gain_right = 3490.0f / 32768.0f;
    constexpr float release_seconds = 0.020f;

    struct Voice {
        bool active = false;
        bool releasing = false;
        double pos = 0.0;
        float envelope = 0.0f;
    };

    std::mutex sound_mutex;
    std::vector<int16_t> pcm; // Empty if no ROM.
    double pcm_step = source_step_at_22050; // sample steps per output sample at 22050 Hz
    bool from_disc = false;   // pcm comes from a Dreamcast source's banks once they load
    constexpr int wing_sound = 0x3D;
    std::array<Voice, rush2::wings::max_cars> voices;

    int16_t clamp16(int32_t v) {
        return int16_t(std::clamp(v, -32768, 32767));
    }

    int16_t be16(const std::vector<uint8_t>& rom, size_t o) {
        return int16_t((rom[o] << 8) | rom[o + 1]);
    }

    std::vector<int16_t> decode(const std::vector<uint8_t>& rom) {
        uint32_t blocks = (sample_length + block_samples - 1) / block_samples;
        if (rom.size() < sample_rom + codebook_size + blocks * block_size) {
            return {};
        }

        int16_t book[8][2][8];
        for (int p = 0; p < 8; p++) {
            for (int order = 0; order < 2; order++) {
                for (int k = 0; k < 8; k++) {
                    book[p][order][k] = be16(rom, sample_rom + ((p * 2 + order) * 8 + k) * 2);
                }
            }
        }

        std::vector<int16_t> out;
        out.reserve(blocks * block_samples);
        for (uint32_t b = 0; b < blocks; b++) {
            size_t block = sample_rom + codebook_size + b * block_size;
            for (int half = 0; half < 2; half++) {
                size_t data = block + 8 + half * 16;
                uint8_t header = rom[data];
                const int16_t* b0 = book[header >> 4][0];
                const int16_t* b1 = book[header >> 4][1];
                int shift = header & 0xF;

                int32_t e[32];
                for (int i = 0; i < 16; i++) {
                    for (int n = 0; n < 2; n++) {
                        int32_t v = n == 0 ? (rom[data + i] >> 4) : (rom[data + i] & 0xF);
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

                int16_t s[32];
                s[0] = be16(rom, block + half * 4);
                s[1] = be16(rom, block + half * 4 + 2);
                // Samples 2-7 continue from the anchors, then three vectors of 8.
                auto run = [&](int first, int count) {
                    int32_t prev2 = s[first - 2], prev1 = s[first - 1];
                    for (int k = 0; k < count; k++) {
                        int32_t acc = b0[k] * prev2 + b1[k] * prev1 + 2048 * e[first + k];
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
        out.resize(sample_length);
        return out;
    }

    float sample_at(const std::vector<int16_t>& data, int64_t i) {
        int64_t n = int64_t(data.size());
        i %= n;
        if (i < 0) {
            i += n;
        }
        return data[size_t(i)];
    }
}

void rush2::wings::reload_sound() {
    std::vector<int16_t> decoded;
    auto rom = get_rom();
    if (rom != nullptr && rom->n64_rom() != nullptr) {
        decoded = decode(*rom->n64_rom());
    }
    std::lock_guard lock{ sound_mutex };
    pcm = std::move(decoded);
    pcm_step = source_step_at_22050;
    from_disc = rom != nullptr && rom->is_dreamcast();
    voices = {};
}

void rush2::wings::set_sound(int car, bool playing) {
    if (car < 0 || car >= max_cars) {
        return;
    }
    std::lock_guard lock{ sound_mutex };
    Voice& v = voices[car];
    if (playing) {
        // A finished voice is freed at key-off, so a new slide starts the sample over.
        if (!v.active || v.releasing) {
            v = Voice{ true, false, 0.0, 1.0f };
        }
    }
    else if (v.active) {
        v.releasing = true;
    }
}

void rush2::wings::mix_sound(float* samples, size_t sample_count, uint32_t sample_rate, float pcm_scale) {
    std::lock_guard lock{ sound_mutex };
    uint32_t rate = 0;
    if (pcm.empty() && from_disc && rush2::audio2049::sfx_samples(wing_sound, pcm, rate)) {
        pcm_step = 0.75 * rate / 22050.0;
    }
    if (pcm.empty() || sample_rate == 0) {
        return;
    }
    const double step = pcm_step * 22050.0 / sample_rate;
    const float release_step = 1.0f / (release_seconds * sample_rate);

    for (Voice& v : voices) {
        if (!v.active) {
            continue;
        }
        for (size_t i = 0; i + 1 < sample_count; i += 2) {
            // Catmull-Rom interpolation across the loop.
            int64_t index = int64_t(std::floor(v.pos));
            float t = float(v.pos - double(index));
            float p0 = sample_at(pcm, index - 1), p1 = sample_at(pcm, index);
            float p2 = sample_at(pcm, index + 1), p3 = sample_at(pcm, index + 2);
            float s = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));

            samples[i + 0] += s * gain_left * v.envelope * pcm_scale;
            samples[i + 1] += s * gain_right * v.envelope * pcm_scale;

            v.pos += step;
            if (v.pos >= double(pcm.size())) {
                v.pos -= double(pcm.size());
            }
            if (v.releasing) {
                v.envelope -= release_step;
                if (v.envelope <= 0.0f) {
                    v = Voice{};
                    break;
                }
            }
        }
    }
}
