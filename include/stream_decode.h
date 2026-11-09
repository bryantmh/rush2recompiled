#ifndef __STREAM_DECODE_H__
#define __STREAM_DECODE_H__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio2049.h"

// Decoders for the streamed songs (audio2049::StreamSong, src/stream_decode.cpp): a song's bytes to 16-bit stereo
// frames, decoded in order as the song player (src/rush2049dc/audio2049_dc.cpp) reaches them.
namespace rush2::audio2049 {
    class StreamDecoder {
    public:
        virtual ~StreamDecoder() = default;
        size_t frames = 0;          // the song's length
        uint32_t rate = 44100;
        size_t loop_frame = 0;      // where it loops back to
        float gain = 1.0f;          // what the player multiplies the frames by (undoes a 5.1 fold's scaling)
        // Decodes up to `count` frames following the last call's into `stereo` (interleaved); returns how many.
        virtual size_t decode(int16_t* stereo, size_t count) = 0;
    };
    // A decoder over a song's bytes (as StreamSong::read gave them), or nullptr (with why in `error`).
    std::unique_ptr<StreamDecoder> open_stream(const StreamSong& song, std::vector<uint8_t> bytes, std::string& error);
}

#endif
