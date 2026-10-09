// Dreamcast source test: imports a disc image into a pack (once), then writes the N64 files the Dreamcast source makes
// so tools/rush2049 can compare them with the N64 ROM's (dc_check.py).
//   dc_test.exe IMAGE PACK OUTDIR [N64 file indices...]   (no indices: all files the disc converts)
// Build with dc_build.bat.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "audio2049.h"
#include "rush2049_dc.h"
#include "track2049_convert.h"

namespace {
    // dc_test track PACK RUSH2ROM K...: runs track2049::convert_track over the Dreamcast source (RUSH2ROM is the recomp
    // ROM with Rush 2's main code uncompressed at 0x01000000, for its shared model names).
    int test_tracks(int argc, char** argv) {
        auto src = rush2::rom2049::dc::open_pack(argv[2]);
        std::ifstream f(argv[3], std::ios::binary);
        std::vector<uint8_t> rom2((std::istreambuf_iterator<char>(f)), {});
        if (!src || rom2.empty()) {
            fprintf(stderr, "can't open the pack or the Rush 2 ROM\n");
            return 1;
        }
        auto be32 = [&](size_t o) {
            return ((uint32_t)rom2[o] << 24) | ((uint32_t)rom2[o + 1] << 16) | ((uint32_t)rom2[o + 2] << 8) | rom2[o + 3];
        };
        auto asset_rom = [&](int index) { return be32(0x01000000 + 0x800C185C - 0x800539E0 + index * 4); };
        std::set<std::string> shared;
        rush2::track2049::rush2_shared_model_names(rom2, asset_rom(0x12), asset_rom(0x14), shared);
        int failed = 0;
        for (int i = 4; i < argc; i++) {
            int k = atoi(argv[i]);
            bool stunt = k >= rush2::track2049::stunt_first && k != rush2::track2049::obstacle;
            rush2::track2049::ConvertedTrack t;
            std::string error;
            bool ok = rush2::track2049::convert_track(*src, k, stunt ? "STUNT1" : "HAWAII", shared, true, t, error);
            printf("track %d: %s geometry 0x%zX placement 0x%zX collision 0x%zX\n", k, ok ? "ok" : error.c_str(),
                   t.geometry.size(), t.placement.size(), t.collision.size());
            failed += !ok;
        }
        return failed ? 1 : 0;
    }
}

namespace {
    void write_wav(const std::filesystem::path& path, const std::vector<float>& s, uint32_t rate) {
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
        auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
        uint32_t bytes = (uint32_t)s.size() * 2;
        f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4);
        u16(4); u16(16); f.write("data", 4); u32(bytes);
        for (float v : s) {
            int16_t x = (int16_t)std::clamp(v * 32767.0f, -32768.0f, 32767.0f);
            f.write((const char*)&x, 2);
        }
    }

    // dc_test audio PACK OUTDIR: every N64 effect id the disc pairs, then 20 s of each race track's song and the
    // menu song, as WAVs at 48 kHz (gains as the port's mixer passes them at full volume).
    int test_audio(char** argv) {
        namespace audio = rush2::audio2049;
        auto src = rush2::rom2049::dc::open_pack(argv[2]);
        std::filesystem::path out = argv[3];
        std::filesystem::create_directories(out);
        if (!src || !audio::load(src)) {
            fprintf(stderr, "can't load the disc's sound\n");
            return 1;
        }
        constexpr uint32_t rate = 48000;
        const float music_gain = 127.0f / 114.0f * 2.8f, sfx_gain = 127.0f / 114.0f * 2.0f;
        int sounds = 0;
        for (int id = 0; id < audio::sfx_count; id++) {
            int h = audio::sfx_start(id);
            if (h < 0) continue;
            std::vector<float> s;
            for (int block = 0; block < 4 * 50 && audio::sfx_active(h); block++) {
                std::vector<float> b(rate / 50 * 2, 0.0f);
                audio::mix(b.data(), rate / 50, rate, 0.0f, sfx_gain);
                s.insert(s.end(), b.begin(), b.end());
            }
            audio::sfx_stop_all();
            char name[32];
            snprintf(name, sizeof(name), "sfx_%02X.wav", id);
            write_wav(out / name, s, rate);
            sounds++;
        }
        printf("%d effects\n", sounds);
        for (int track : { 0, 1, 2, 3, 4, 5, 14, 18, -1 }) {
            audio::set_track(track);
            audio::play_song(track < 0 ? 6 : audio::track_song(track) >= 0 ? audio::track_song(track) : 8);
            for (int i = 0; i < 400 && audio::stats().song >= 0; i++) {
                std::vector<float> probe(2, 0.0f);
                audio::mix(probe.data(), 1, rate, 0.0f, 0.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::vector<float> s(rate * 20 * 2, 0.0f);
            audio::mix(s.data(), rate * 20, rate, music_gain, 0.0f);
            double sum = 0, peak = 0;
            for (float v : s) { sum += v * v; peak = std::max(peak, (double)std::fabs(v)); }
            printf("track %d: song %d, rms %.1f dBFS, peak %.1f dBFS\n", track, audio::current_song(),
                   10 * std::log10(sum / s.size() + 1e-12), 20 * std::log10(peak + 1e-12));
            char name[32];
            snprintf(name, sizeof(name), "song_track%d.wav", track);
            write_wav(out / name, s, rate);
            audio::stop_song(0.0f);
        }
        return 0;
    }
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "track") return test_tracks(argc, argv);
    if (argc >= 4 && std::string(argv[1]) == "audio") return test_audio(argv);
    if (argc < 4) {
        fprintf(stderr, "dc_test IMAGE PACK OUTDIR [indices]\n");
        return 2;
    }
    namespace fs = std::filesystem;
    fs::path image = argv[1], pack = argv[2], outdir = argv[3];
    if (!fs::exists(pack)) {
        auto r = rush2::rom2049::dc::import(image, pack);
        if (r != rush2::rom2049::dc::ImportResult::Good) {
            fprintf(stderr, "import failed: %d\n", (int)r);
    if (argc >= 4 && std::string(argv[1]) == "segments") {
        // dc_test segments PACK|ROM.z64 OUTDIR: the source's code segments as seg0.bin.. (what the game reads its
        // tables from; tools/rush2049/dc_tables.py --compare OUTDIR checks a disc's against the tool).
        std::filesystem::path in_path = argv[2];
        std::shared_ptr<const rush2::rom2049::Source> src;
        if (in_path.extension() == ".z64") {
            std::ifstream in(in_path, std::ios::binary);
            auto rom = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            src = rush2::rom2049::n64_source(rom);
        }
        else {
            src = rush2::rom2049::dc::open_pack(in_path);
        }
        if (!src) {
            fprintf(stderr, "can't open %s\n", argv[2]);
            return 1;
        }
        std::filesystem::create_directories(argv[3]);
        for (int s = 0; s < 3; s++) {
            auto seg = src->segment((rush2::rom2049::Segment)s);
            if (!seg) continue;
            std::ofstream(std::filesystem::path(argv[3]) / ("seg" + std::to_string(s) + ".bin"), std::ios::binary)
                .write((const char*)seg->data(), seg->size());
        }
        return 0;
    }
            return 1;
        }
    }
    auto src = rush2::rom2049::dc::open_pack(pack);
    if (!src) {
        fprintf(stderr, "open_pack failed\n");
        return 1;
    }
    fs::create_directories(outdir);
    std::vector<int> indices;
    for (int i = 4; i < argc; i++) indices.push_back(atoi(argv[i]));
    if (indices.empty()) {
        for (int i = 0; i < rush2::rom2049::file_count; i++) indices.push_back(i);
    }
    int failed = 0;
    for (int i : indices) {
        std::vector<uint8_t> out;
        if (!src->read_file(i, out)) {
            if (argc > 4) {
                printf("%d: no file\n", i);
                failed++;
            }
            continue;
        }
        char name[16];
        snprintf(name, sizeof(name), "%03d.bin", i);
        std::ofstream(outdir / name, std::ios::binary).write((const char*)out.data(), out.size());
        printf("%d: %zu bytes\n", i, out.size());
    }
    for (auto s : { rush2::rom2049::Segment::Boot, rush2::rom2049::Segment::Main, rush2::rom2049::Segment::Battle }) {
        auto seg = src->segment(s);
        printf("segment %d: %s\n", (int)s, seg ? "built" : "missing");
        if (seg) {
            char name[16];
            snprintf(name, sizeof(name), "seg%d.bin", (int)s);
            std::ofstream(outdir / name, std::ios::binary).write((const char*)seg->data(), seg->size());
        }
    }
    return failed ? 1 : 0;
}
