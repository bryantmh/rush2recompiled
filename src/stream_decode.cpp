// Decoders for the streamed songs (include/stream_decode.h): CD audio, the 2049 arcade's 5.1 streams, WAV and Ogg Vorbis.
//
// 5.1 folds to stereo the usual way (ITU-R BS.775): left = FL + 0.707 C + 0.707 BL, right = FR + 0.707 C + 0.707 BR,
// the LFE left out (the arcade's is silent). The arcade's channels carry different parts (the centre and rears hold
// their own instruments, uncorrelated with the fronts), so keeping only the fronts would lose them. The fold is scaled
// by 1 / (1 + 2 x 0.707) to fit 16 bits, and the decoder's gain puts that back for the player.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "stream_decode.h"

// stb_vorbis (public domain), the copy in rt64's tree, compiled here: rt64 doesn't build it.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../lib/rt64/src/contrib/stb/stb_vorbis.c"

namespace {
    using rush2::audio2049::StreamDecoder;
    using rush2::audio2049::StreamSong;

    constexpr float fold_side = 0.70710678f;
    constexpr float fold_scale = 1.0f / (1.0f + 2.0f * fold_side);

    int16_t clamp16(float v) {
        return (int16_t)std::clamp((int)std::lround(v), -32768, 32767);
    }

    // Interleaved samples of `channels` channels (1, 2 or 6) as floats in -32768..32767, to stereo.
    struct Channels {
        int channels = 2;
        void to_stereo(const float* in, int16_t* out) const {
            if (channels == 1) {
                out[0] = out[1] = clamp16(in[0]);
            }
            else if (channels == 6) {
                float c = in[2] * fold_side;
                out[0] = clamp16((in[0] + c + in[4] * fold_side) * fold_scale);
                out[1] = clamp16((in[1] + c + in[5] * fold_side) * fold_scale);
            }
            else {
                out[0] = clamp16(in[0]);
                out[1] = clamp16(in[1]);
            }
        }
        float gain() const {
            return channels == 6 ? 1.0f / fold_scale : 1.0f;
        }
    };

    // Raw PCM: `bits` per sample (8 unsigned, 16/24/32 signed, or 32-bit float), little endian.
    class PcmDecoder : public StreamDecoder {
    public:
        PcmDecoder(std::vector<uint8_t> bytes, size_t start, size_t length, int channels, int bits, bool is_float)
            : data(std::move(bytes)), pos(start), ch{ channels }, bits(bits), is_float(is_float) {
            size_t frame = (size_t)channels * (bits / 8);
            frames = std::min(length, data.size() - std::min(start, data.size())) / frame;
            gain = ch.gain();
        }

        size_t decode(int16_t* stereo, size_t count) override {
            size_t frame = (size_t)ch.channels * (bits / 8);
            size_t n = std::min(count, frames - done);
            float in[6];
            for (size_t k = 0; k < n; k++, pos += frame) {
                const uint8_t* p = &data[pos];
                for (int c = 0; c < ch.channels; c++) {
                    in[c] = sample(p + c * (bits / 8));
                }
                ch.to_stereo(in, stereo + k * 2);
            }
            done += n;
            if (done >= frames) {
                data = {};
            }
            return n;
        }

    private:
        std::vector<uint8_t> data;
        size_t pos = 0, done = 0;
        Channels ch;
        int bits;
        bool is_float;

        float sample(const uint8_t* p) const {
            switch (bits) {
                case 8: return ((int)p[0] - 128) * 256.0f;
                case 16: return (float)(int16_t)(p[0] | (p[1] << 8));
                case 24: return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16);
                default: {
                    uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
                    if (is_float) {
                        float f;
                        memcpy(&f, &u, 4);
                        return f * 32768.0f;
                    }
                    return (float)((int32_t)u >> 16);
                }
            }
        }
    };

    class OggDecoder : public StreamDecoder {
    public:
        ~OggDecoder() override {
            if (v != nullptr) stb_vorbis_close(v);
        }
        bool open(std::vector<uint8_t> bytes, std::string& error) {
            data = std::move(bytes);
            int err = 0;
            v = stb_vorbis_open_memory(data.data(), (int)data.size(), &err, nullptr);
            if (v == nullptr) {
                error = "not an Ogg Vorbis file (stb_vorbis error " + std::to_string(err) + ")";
                return false;
            }
            stb_vorbis_info info = stb_vorbis_get_info(v);
            rate = info.sample_rate;
            frames = stb_vorbis_stream_length_in_samples(v);
            if (frames == 0 || rate == 0) {
                error = "an Ogg Vorbis file with no samples";
                return false;
            }
            // LOOPSTART=<frame>, the comment game music mods use for where a song loops back to.
            stb_vorbis_comment c = stb_vorbis_get_comment(v);
            for (int i = 0; i < c.comment_list_length; i++) {
                const char* s = c.comment_list[i];
                if (strncmp(s, "LOOPSTART=", 10) == 0 || strncmp(s, "loopstart=", 10) == 0) {
                    size_t f = (size_t)strtoull(s + 10, nullptr, 10);
                    if (f < frames) loop_frame = f;
                }
            }
            return true;
        }

        size_t decode(int16_t* stereo, size_t count) override {
            size_t got = 0;
            while (got < count && done < frames) {
                int n = stb_vorbis_get_samples_short_interleaved(v, 2, stereo + got * 2,
                                                                 (int)std::min<size_t>(count - got, 4096) * 2);
                if (n <= 0) {
                    frames = done; // the stream ended early: what was decoded is the song
                    break;
                }
                got += (size_t)n;
                done += (size_t)n;
            }
            if (done >= frames && v != nullptr) {
                stb_vorbis_close(v);
                v = nullptr;
                data = {};
            }
            return got;
        }

    private:
        std::vector<uint8_t> data;
        stb_vorbis* v = nullptr;
        size_t done = 0;
    };

    uint32_t le32(const std::vector<uint8_t>& d, size_t o) {
        return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    }
    uint16_t le16(const std::vector<uint8_t>& d, size_t o) {
        return (uint16_t)(d[o] | (d[o + 1] << 8));
    }

    std::unique_ptr<StreamDecoder> open_wav(std::vector<uint8_t> d, std::string& error) {
        if (d.size() < 12 || memcmp(&d[0], "RIFF", 4) != 0 || memcmp(&d[8], "WAVE", 4) != 0) {
            error = "not a WAV file";
            return nullptr;
        }
        int format = 0, channels = 0, bits = 0;
        uint32_t rate = 0;
        size_t data_at = 0, data_size = 0;
        for (size_t o = 12; o + 8 <= d.size();) {
            uint32_t size = le32(d, o + 4);
            if (memcmp(&d[o], "fmt ", 4) == 0 && size >= 16 && o + 8 + 16 <= d.size()) {
                format = le16(d, o + 8);
                channels = le16(d, o + 10);
                rate = le32(d, o + 12);
                bits = le16(d, o + 22);
                if (format == 0xFFFE && size >= 40 && o + 8 + 26 <= d.size()) {
                    format = le16(d, o + 8 + 24);   // WAVE_FORMAT_EXTENSIBLE: the subformat's first word
                }
            }
            else if (memcmp(&d[o], "data", 4) == 0) {
                data_at = o + 8;
                data_size = std::min<size_t>(size, d.size() - data_at);
                break;
            }
            o += 8 + size + (size & 1);
        }
        bool is_float = format == 3 && bits == 32;
        if (!(format == 1 || is_float) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32) ||
            !(channels == 1 || channels == 2 || channels == 6) || rate == 0 || data_at == 0) {
            error = "a WAV file this player can't read (it reads 8/16/24/32-bit PCM or 32-bit float, mono, stereo or 5.1)";
            return nullptr;
        }
        auto dec = std::make_unique<PcmDecoder>(std::move(d), data_at, data_size, channels, bits, is_float);
        dec->rate = rate;
        return dec;
    }
}

std::unique_ptr<StreamDecoder> rush2::audio2049::open_stream(const StreamSong& song, std::vector<uint8_t> bytes,
                                                            std::string& error) {
    if (song.open) {
        auto dec = song.open();
        if (!dec) error = "its data isn't loaded";
        return dec;
    }
    switch (song.format) {
        case StreamSong::Format::pcm16_stereo:
        case StreamSong::Format::pcm16_surround: {
            int channels = song.format == StreamSong::Format::pcm16_surround ? 6 : 2;
            if (bytes.size() <= song.skip_bytes) {
                error = "no frames";
                return nullptr;
            }
            size_t length = bytes.size() - song.skip_bytes;
            auto dec = std::make_unique<PcmDecoder>(std::move(bytes), song.skip_bytes, length, channels, 16, false);
            dec->rate = song.rate;
            dec->loop_frame = song.loop_frame < dec->frames ? song.loop_frame : 0;
            return dec;
        }
        case StreamSong::Format::wav:
            return open_wav(std::move(bytes), error);
        case StreamSong::Format::ogg: {
            auto dec = std::make_unique<OggDecoder>();
            if (!dec->open(std::move(bytes), error)) return nullptr;
            return dec;
        }
    }
    error = "unknown format";
    return nullptr;
}
