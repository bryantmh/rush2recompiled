// Rush 2049's music and moving-object sounds on the 2049 tracks (engine: src/rush2049/audio2049.cpp).
//
// Music: src/music.cpp picks each race's song. Rush 2 sends its music thread commands through func_80062F50:
// (sequence << 16) | 0xFFFF plays a sequence, 0x40000000 stops, 0xC0000000 applies the fade factor at 0x800BD150.
// The volume is (byte 0x800D5773 / 40) x fade at full scale. For a Rush 2049 song, src/music.cpp queues it here and
// turns the race's music call into Rush 2's "music off" case, whose stop command then starts the 2049 song instead;
// any later play or stop command (results jingle, menus, quitting) ends it, and fade commands follow along.
//
// Object sounds: the follower logic (src/rush2049/track2049_movers_logic.cpp) reports each moving object's sound requests
// (start, loop, stop: type table +0x1C/+0x20/+0x24); src/rush2049/track2049_movers.cpp passes them here with the object's
// position, and they play as positional Rush 2049 sound effects, heard from player 1's car with Rush 2049's emitter
// law (audio2049::emitter_mix).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "recomp.h"
#include "rush2_hooks.h"
#include "audio2049.h"
#include "music.h"
#include "track2049.h"
#include "wings.h"

namespace audio = rush2::audio2049;

namespace {
    constexpr uint32_t music_volume = 0x800D5773;   // s8: 0-40
    constexpr uint32_t sfx_volume = 0x800D5774;     // s8: 0-40
    constexpr uint32_t music_fade = 0x800BD150;     // f32
    constexpr uint32_t cameras = 0x800E79D0;        // Per-view cameras, 0x40 bytes: matrix rows +0x0, +0xC, +0x18;
                                                    // position +0x24.
    constexpr uint32_t player_car = 0x800F5470;      // Car 0's physics struct; +0x224 = position.
    constexpr uint32_t cmd_stop = 0x40000000;

    std::mutex load_mutex;
    std::shared_ptr<const rush2::rom2049::Source> loaded_rom;
    std::atomic<bool> loaded = false;
    std::atomic<bool> loading = false;

    // The 2049 track id (0-18: race tracks, battle arenas 1-8, stunt arenas 1-4, obstacle course; the index of the
    // per-track song tables) being raced, or -1.
    int raced_track_id() {
        int k = rush2::track2049::race_track();
        if (k == rush2::track2049::obstacle) return 18;
        if (k >= 1 && k <= 6) return k - 1;
        if (int n = rush2::track2049::battle_arena()) return 5 + n;
        if (int n = rush2::track2049::stunt_arena()) return 13 + n;
        return -1;
    }

    std::atomic<int> pending_song = -1;
    std::atomic<float> music_gain = 0.0f;
    std::atomic<float> sfx_gain = 0.0f;
    // When object sounds were last updated: they go quiet while the race is paused (no physics ticks).
    std::atomic<int64_t> sounds_updated_ms = 0;
    std::atomic<uint8_t*> sounds_rdram = nullptr;

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    float read_f(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    // Loads the sound banks from the 2049 ROM on a worker thread, once per ROM.
    bool ready() {
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return false;
        }
        std::lock_guard lock{ load_mutex };
        if (loaded_rom != rom && !loading) {
            loaded_rom = rom;
            loaded = false;
            loading = true;
            std::thread([rom]() {
                bool ok = audio::load(rom);
                if (!ok) {
                    fprintf(stderr, "[2049] Couldn't load Rush 2049's sound banks\n");
                }
                loaded = ok;
                loading = false;
            }).detach();
        }
        return loaded;
    }

    void update_gains(uint8_t* rdram) {
        sounds_rdram = rdram;
        float fade = read_f(rdram, music_fade);
        if (!(fade >= 0.0f && fade <= 1.0f)) {
            fade = 1.0f;
        }
        // audio2049's gain 1 is Rush 2049's default mix (master volume 114/127); Rush 2's full volume (40) maps to
        // Rush 2049's full volume.
        constexpr float full = 127.0f / 114.0f;
        // Music and effects get back the 6 dB of headroom audio2049 leaves (its full voice gain is 0.5). Music gets
        // 3 dB more: at 2x alone it still sounded quieter than Rush 2's own. Effects (2049 engines among them) stay at
        // 2x; at 2.8x the engines were too loud. Peaks are soft-limited in mix_audio.
        constexpr float music_boost = 2.8f;
        constexpr float sfx_boost = 2.0f;
        music_gain = (int8_t)MEM_B(0, (int32_t)music_volume) / 40.0f * full * music_boost * fade;
        sfx_gain = (int8_t)MEM_B(0, (int32_t)sfx_volume) / 40.0f * full * sfx_boost;
    }

    // Per moving object: the sound effect it plays and the loop it waits to start.
    struct ObjectVoice {
        int handle = -1;
        int pending_loop = -1;
        int loop_id = -1;   // Sound id of the loop playing, or -1.
        int pending_stop = -2; // Stop sound (-1 none) waiting for the start sound to finish, or -2.
    };
    std::mutex objects_mutex;
    std::vector<ObjectVoice> object_voices;
}

bool rush2::track2049::music_ready() {
    return ready();
}

void rush2::track2049::queue_race_song(int song) {
    pending_song = song;
}

int rush2::track2049::queued_race_song() {
    return pending_song;
}

void rush2::track2049::play_song_now(uint8_t* rdram, int song) {
    if (!ready()) {
        return;
    }
    update_gains(rdram);
    audio::set_track(-1);
    audio::play_song(song);
}

void rush2::track2049::stop_song_now() {
    if (loaded) {
        audio::stop_song(0.0f);
    }
}

int rush2::track2049::playing_song() {
    return loaded && audio::song_playing() ? audio::current_song() : -1;
}

// func_80062F50 entry: queues music command $a0.
extern "C" void rush2_track49_music_command(uint8_t* rdram, recomp_context* ctx) {
    rush2::music::game_command(rdram, ctx); // May turn a play or stop into a volume update while a preview plays.
    uint32_t cmd = (uint32_t)ctx->r4;
    if (cmd == cmd_stop) {
        int song = pending_song.exchange(-1);
        if (song >= 0) {
            update_gains(rdram);
            audio::set_track(raced_track_id());
            audio::play_song(song);
            fprintf(stderr, "[2049] Playing Rush 2049 song %d\n", song);
        }
        else if (loaded) {
            audio::stop_song(0.5f);
            rush2::track2049::stop_object_sounds();
        }
    }
    else if ((cmd >> 30) == 3) {
        update_gains(rdram);
    }
    else if ((cmd & 0xFFFF) == 0xFFFF && (cmd >> 31) == 0 && loaded) {
        audio::stop_song(0.0f);
    }
}

void rush2::track2049::update_object_sounds(uint8_t* rdram, const std::vector<ObjectSound>& sounds) {
    std::lock_guard lock{ objects_mutex };
    bool on = ready();
    if (object_voices.size() != sounds.size()) {
        for (ObjectVoice& v : object_voices) {
            if (v.handle >= 0 && loaded) audio::sfx_stop(v.handle);
        }
        object_voices.assign(sounds.size(), ObjectVoice{});
    }
    update_gains(rdram);
    sounds_updated_ms = now_ms();
    // Listener: player 1's car, with player 1's camera orientation (Rush 2049 hears emitters from the player's car).
    float cam[9], car[3];
    for (int i = 0; i < 9; i++) cam[i] = read_f(rdram, cameras + i * 4);
    for (int i = 0; i < 3; i++) car[i] = read_f(rdram, player_car + 0x224 + i * 4);
    const float* up = &cam[3];
    const float* back = &cam[6];
    for (size_t i = 0; i < sounds.size(); i++) {
        const ObjectSound& s = sounds[i];
        ObjectVoice& v = object_voices[i];
        if (!on) {
            if (v.handle >= 0 && loaded) audio::sfx_stop(v.handle);
            v = ObjectVoice{};
            continue;
        }
        // Loop levels (func_800BF45C): sound 1 (trains) follows the object's speed, 0x12 (flags) and 0x61 (barge)
        // are fixed; others play at full volume and pitch.
        int loop_id = v.loop_id;
        float max_volume = 1.0f, pitch = 1.0f;
        if (loop_id == 0x01) { max_volume = s.speed_factor * 0.75f + 0.25f; pitch = s.speed_factor; }
        else if (loop_id == 0x12) { max_volume = 0.8f; }
        else if (loop_id == 0x61) { pitch = 0.75f; }
        audio::EmitterParams e = audio::emitter_mix(s.pos, car, back, up, s.range, max_volume);
        float volume = e.volume, pan = e.pan, surround = e.surround;

        switch (s.request) {
            case ObjectSound::start:
                if (v.handle >= 0) audio::sfx_stop(v.handle);
                v.handle = s.ids[0] >= 0 ? audio::sfx_start(s.ids[0], volume, pan, 1.0f, surround) : -1;
                v.pending_loop = -1;
                v.loop_id = -1;
                v.pending_stop = -2;
                break;
            case ObjectSound::loop:
                v.pending_loop = s.ids[1];
                v.pending_stop = -2;
                break;
            case ObjectSound::stop:
                // func_800BF1C8 stops only once the start sound has finished (TRIGGER pads' click).
                if (v.handle >= 0 && v.loop_id < 0 && audio::sfx_active(v.handle)) {
                    v.pending_stop = s.ids[2];
                    v.pending_loop = -1;
                    break;
                }
                if (v.handle >= 0) audio::sfx_stop(v.handle);
                v.handle = s.ids[2] >= 0 ? audio::sfx_start(s.ids[2], volume, pan, 1.0f, surround) : -1;
                v.pending_loop = -1;
                v.loop_id = -1;
                break;
            default:
                break;
        }
        if (v.pending_stop != -2 && (v.handle < 0 || !audio::sfx_active(v.handle))) {
            v.handle = v.pending_stop >= 0 ? audio::sfx_start(v.pending_stop, volume, pan, 1.0f, surround) : -1;
            v.pending_stop = -2;
        }
        // The loop starts once the start sound has finished (func_800BF45C).
        if (v.pending_loop >= 0 && (v.handle < 0 || !audio::sfx_active(v.handle))) {
            v.handle = audio::sfx_start(v.pending_loop, volume, pan, pitch, surround);
            v.loop_id = v.pending_loop;
            v.pending_loop = -1;
        }
        if (v.handle >= 0) {
            audio::sfx_update(v.handle, volume, pan, v.loop_id >= 0 ? pitch : 1.0f, surround);
        }
    }
}

void rush2::track2049::play_effect(uint8_t* rdram, int id, const float pos[3], float range) {
    if (id < 0 || !ready()) {
        return;
    }
    update_gains(rdram);
    sounds_updated_ms = now_ms();
    float cam[9], car[3];
    for (int i = 0; i < 9; i++) cam[i] = read_f(rdram, cameras + i * 4);
    for (int i = 0; i < 3; i++) car[i] = read_f(rdram, player_car + 0x224 + i * 4);
    audio::EmitterParams e = audio::emitter_mix(pos, car, &cam[6], &cam[3], range);
    if (e.volume > 0.0f) {
        audio::sfx_start(id, e.volume, e.pan, 1.0f, e.surround);
    }
}

void rush2::track2049::effects_running(uint8_t* rdram) {
    update_gains(rdram);
    sounds_updated_ms = now_ms();
}

bool rush2::track2049::sounds_paused(uint8_t* rdram, int64_t updated_ms) {
    constexpr uint32_t pause_state = 0x8002305C; // Nonzero while the pause menu (or one of its screens) is open.
    int64_t idle = now_ms() - updated_ms;
    return idle > 1000 || (idle > 100 && rdram != nullptr && MEM_B(0, (int32_t)pause_state) != 0);
}

void rush2::track2049::stop_object_sounds() {
    std::lock_guard lock{ objects_mutex };
    for (ObjectVoice& v : object_voices) {
        if (v.handle >= 0 && loaded) audio::sfx_stop(v.handle);
    }
    object_voices.clear();
}

void rush2::track2049::mix_audio(float* samples, size_t sample_count, uint32_t sample_rate, float scale) {
    if (!loaded) {
        return;
    }
    bool paused = sounds_paused(sounds_rdram.load(), sounds_updated_ms.load());
    audio::mix(samples, sample_count / 2, sample_rate, music_gain.load() * scale, paused ? 0.0f : sfx_gain.load() * scale);
    // Loud passages can pass full scale: a soft knee above 0.9 keeps them from clipping hard.
    constexpr float knee = 0.9f, headroom = 1.0f - knee;
    for (size_t i = 0; i < sample_count; i++) {
        float a = std::fabs(samples[i]);
        if (a > knee) {
            samples[i] = std::copysign(knee + headroom * std::tanh((a - knee) / headroom), samples[i]);
        }
    }
}
