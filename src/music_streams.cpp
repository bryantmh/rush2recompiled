// The songs streamed from files (include/music_streams.h): where each source's songs are and how they're read.
//
// The Rush 2049 arcade's AUDIO/*.str (docs/arcade2049.md#music): 'STRM', u32 data bytes, u32 0x36, u32 0x52, zeros to
// 512 bytes; the whole file is 5.1 PCM16 frames (12 bytes: FL FR C LFE BL BR) at 22050 Hz counted from byte 0, so
// the first whole frame after the header is frame 43 (byte 516). A song loops from its start.
// The PlayStation disc's CD audio: 16-bit stereo at 44.1 kHz, where the .cue puts each track (sfrush_ps1::audio_tracks).
// A mod's song file is read from the mod (its .rtz or folder) when the song starts.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>

#include "music_streams.h"
#include "audio2049.h"
#include "arcade2049.h"
#include "sfrush_ps1.h"
#include "rock_music.h"
#include "texture_upscale.h"

namespace audio = rush2::audio2049;
namespace ms = rush2::music_streams;

namespace {
    constexpr size_t arcade_first_frame = 516;   // 43 frames of 12 bytes: the first whole frame after the header
    constexpr uint32_t arcade_rate = 22050;

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // A file's bytes from `offset`, `length` of them (0: to the end).
    bool read_range(const std::filesystem::path& path, uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        uint64_t size = (uint64_t)f.tellg();
        if (offset > size) return false;
        if (length == 0 || offset + length > size) length = size - offset;
        out.resize((size_t)length);
        f.seekg((std::streamoff)offset);
        f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)length);
        return (bool)f;
    }

    // The sources' registrations, so a refresh only touches what changed.
    std::mutex sources_mutex;
    std::filesystem::path arcade_dir;
    std::filesystem::path ps1_disc;
    std::filesystem::path rock_dir;

    // Rush The Rock's songs: synthesized as they play from its sound ROMs (src/arcade2049/rock_music.cpp).
    void register_rock(const std::filesystem::path& dir) {
        bool ok = !dir.empty() && rush2::rock_music::load(dir);
        for (int n = 0; n < rush2::rock_music::song_count; n++) {
            if (!ok) {
                audio::set_stream_song(audio::rock_songs + n, nullptr);
                continue;
            }
            auto s = std::make_shared<audio::StreamSong>();
            s->open = [n]() { return rush2::rock_music::open(n); };
            s->gain = audio::rock_music_compensation;
            audio::set_stream_song(audio::rock_songs + n, s);
        }
    }

    void register_arcade(const std::filesystem::path& dir) {
        std::error_code ec;
        for (int n = 0; n < ms::arcade_song_count; n++) {
            std::string name = n == ms::arcade_track8 ? "therock.str"
                             : n == ms::arcade_select ? "select.str"
                             : "track" + std::to_string(n + 1) + ".str";
            std::filesystem::path file = dir / "AUDIO" / name;
            if (dir.empty() || !std::filesystem::is_regular_file(file, ec)) {
                audio::set_stream_song(audio::arcade2049_songs + n, nullptr);
                continue;
            }
            auto s = std::make_shared<audio::StreamSong>();
            s->format = audio::StreamSong::Format::pcm16_surround;
            s->read = [file](std::vector<uint8_t>& out) { return read_range(file, 0, 0, out); };
            s->skip_bytes = arcade_first_frame;
            s->rate = arcade_rate;
            s->gain = audio::arcade2049_music_compensation;
            audio::set_stream_song(audio::arcade2049_songs + n, s);
        }
    }

    void register_ps1(const std::filesystem::path& disc) {
        std::vector<rush2::sfrush_ps1::AudioTrack> tracks;
        if (!disc.empty()) tracks = rush2::sfrush_ps1::audio_tracks(disc);
        for (int n = 0; n < ms::ps1_song_count; n++) {
            if (n >= (int)tracks.size()) {
                audio::set_stream_song(audio::ps1_songs + n, nullptr);
                continue;
            }
            rush2::sfrush_ps1::AudioTrack t = tracks[(size_t)n];
            auto s = std::make_shared<audio::StreamSong>();
            s->format = audio::StreamSong::Format::pcm16_stereo;
            s->read = [t](std::vector<uint8_t>& out) { return rush2::sfrush_ps1::read_audio(t, out); };
            s->rate = 44100;
            s->gain = audio::ps1_music_compensation;
            audio::set_stream_song(audio::ps1_songs + n, s);
        }
    }

    // Mod songs: a song id per file (mod file, its path in the mod), kept for the session.
    std::mutex mods_mutex;
    std::map<std::pair<std::string, std::string>, int> mod_ids;
    int next_mod_id = audio::mod_songs;
    std::vector<ms::ModSong> mod_list;
    std::string mod_list_key;   // what mod_list was made from

    // Registers a custom track's song file `entry`; its id. Under mods_mutex.
    int register_mod_file(const rush2::track_mod::Entry& e, const std::string& entry) {
        std::string inner = e.dir + entry;
        std::string mod_file = rush2::upscale::path_utf8(e.mod_path);
        auto key = std::make_pair(mod_file, inner);
        auto it = mod_ids.find(key);
        if (it != mod_ids.end()) return it->second;
        int id = next_mod_id++;
        mod_ids[key] = id;
        auto s = std::make_shared<audio::StreamSong>();
        s->format = lower(entry).ends_with(".wav") ? audio::StreamSong::Format::wav : audio::StreamSong::Format::ogg;
        std::filesystem::path mod_path = e.mod_path;
        s->read = [mod_path, inner](std::vector<uint8_t>& out) {
            rush2::upscale::ZipReader zip(mod_path);
            if (zip.is_open()) return zip.read(inner, out) && !out.empty();
            out = rush2::upscale::read_file(mod_path / std::filesystem::u8path(inner));
            return !out.empty();
        };
        audio::set_stream_song(id, s);
        return id;
    }
}

bool ms::is_song_file(const std::string& entry) {
    std::string l = lower(entry);
    return l.ends_with(".ogg") || l.ends_with(".wav");
}

void ms::refresh_sources() {
    std::filesystem::path arcade = rush2::arcade2049::current_data_dir(rush2::arcade2049::rush2049_arcade);
    std::filesystem::path disc = rush2::sfrush_ps1::disc();
    std::filesystem::path rock = rush2::arcade2049::current_data_dir(rush2::arcade2049::sfrush_arcade);
    std::lock_guard lock{ sources_mutex };
    static bool first = true;
    if (first || arcade != arcade_dir) {
        arcade_dir = arcade;
        register_arcade(arcade);
    }
    if (first || disc != ps1_disc) {
        ps1_disc = disc;
        register_ps1(disc);
    }
    if (first || rock != rock_dir) {
        rock_dir = rock;
        register_rock(rock);
    }
    first = false;
}

std::vector<int> ms::track_song_ids(const rush2::track_mod::Entry& e) {
    std::lock_guard lock{ mods_mutex };
    std::vector<int> ids;
    for (const std::string& entry : e.settings.music_list) {
        if (is_song_file(entry)) ids.push_back(register_mod_file(e, entry));
    }
    return ids;
}

std::vector<ms::ModSong> ms::mod_songs() {
    std::vector<rush2::track_mod::Entry> tracks = rush2::track_mod::tracks();
    std::string key;
    for (const auto& e : tracks) {
        key += rush2::upscale::path_utf8(e.mod_path) + "|" + e.dir;
        for (const std::string& m : e.settings.music_list) key += "|" + m;
        key += ";";
    }
    std::lock_guard lock{ mods_mutex };
    if (key == mod_list_key) return mod_list;
    mod_list_key = key;
    mod_list.clear();
    for (const auto& e : tracks) {
        for (const std::string& entry : e.settings.music_list) {
            if (!is_song_file(entry)) continue;
            std::string stem = rush2::upscale::path_utf8(std::filesystem::u8path(entry).stem());
            mod_list.push_back({ register_mod_file(e, entry), stem, e.settings.name, e.mod_id + "/" + e.folder });
        }
    }
    return mod_list;
}
