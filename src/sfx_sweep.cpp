// Test aid: RUSH2_TEST_SFX_SWEEP=rush2|2049 plays every sound effect of one game on its own, at full volume, centred,
// through that game's real path, so the two games' effect levels can be compared (issue #51,
// docs/rush2049_research/audio.md#10-loudness-against-rush-2). Rush 2's effects go through the game itself
// (snd_play_effect, libaudio's sound player and the audio microcode), Rush 2049's through audio2049 (the N64's MusyX
// port, or a Dreamcast disc's player when that is the 2049 source).
//
// Run it in a simulation run with RUSH2_AUDIO_LEVELS=<folder> (src/main.cpp writes each source's stream there), from
// the main menu (tools/test_race.py menus --script "8:START:0.3"). It waits RUSH2_TEST_SFX_SWEEP_DELAY seconds (default
// 14) after the audio thread first runs, stops the music, then gives each effect `step` seconds: it stops the last
// one and starts the next. Each start is printed as "[sweep] <game> 0x<id> frame <n>", n being the stream frame it
// starts at, and the engine sounds as "[sweep] <game> engine 0x<id>", for tools/audio_loudness.py --sweep. Rush
// 2049's engine sounds play on its engine bus (audio2049::sfx_set_engine), as its engines do.
// RUSH2_TEST_SFX_SWEEP_FIRST=<id> starts at that id, RUSH2_TEST_SFX_SWEEP_COUNT=<n> stops before id n.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "audio2049.h"
#include "engine2049.h"
#include "sfx_sweep.h"
#include "track2049.h"
#include "wings.h"

extern "C" void snd_play_effect_80062264(uint8_t* rdram, recomp_context* ctx);
extern "C" void snd_stop_and_set_sound_800620EC(uint8_t* rdram, recomp_context* ctx);
extern "C" void music_stop_song_80061E10(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr double step = 1.5;            // seconds per effect
    constexpr int rush2_effects = 0x74;     // snd_play_effect's ids (0x74 is SF Rush's fireworks, added by the port)
    constexpr uint32_t rush2_engine_sounds = 0x800BD1FC;  // s32 [10][2] (src/car_engines.cpp)

    enum class Game { None, Rush2, Rush2049 };

    Game game() {
        static const Game g = [] {
            const char* v = std::getenv("RUSH2_TEST_SFX_SWEEP");
            if (v == nullptr) return Game::None;
            return std::strcmp(v, "rush2") == 0 ? Game::Rush2 : std::strcmp(v, "2049") == 0 ? Game::Rush2049 : Game::None;
        }();
        return g;
    }
}

void rush2::sfx_sweep::audio_frame(uint8_t* rdram, recomp_context* ctx) {
    Game g = game();
    if (g == Game::None) return;
    static double delay = std::getenv("RUSH2_TEST_SFX_SWEEP_DELAY") ? std::atof(std::getenv("RUSH2_TEST_SFX_SWEEP_DELAY")) : 14.0;
    static int next = -1;          // the effect to start next; -1 before the sweep
    static std::set<int> ids;      // Rush 2049's engine sounds: played on the engine bus, as its engines are
    static uint64_t next_frame = 0; // dump frame to start it at
    static int handle = -1;         // the 2049 effect playing
    uint64_t frame = rush2::sfx_sweep::dump_frames();
    if (g == Game::Rush2049) {
        rush2::track2049::effects_running(rdram); // keeps the 2049 effects mixed (they go quiet when nothing reports them)
    }
    if (next < 0) {
        if (frame == 0 || frame < uint64_t(delay * rush2::sfx_sweep::dump_rate())) return;
        if (g == Game::Rush2049 && !(rush2::track2049::music_ready() && rush2::audio2049::loaded())) return;
        // RUSH2_TEST_SFX_SWEEP_FIRST: start at this id (e.g. 0x66, Rush 2049's engine sounds).
        next = std::getenv("RUSH2_TEST_SFX_SWEEP_FIRST") ? int(std::strtol(std::getenv("RUSH2_TEST_SFX_SWEEP_FIRST"), nullptr, 0)) : 0;
        next_frame = frame;
        if (g == Game::Rush2) {
            for (int i = 0; i < 20; i++) {
                int id = (int32_t)MEM_W(0, (int32_t)(rush2_engine_sounds + i * 4));
                if (id >= 0) printf("[sweep] rush2 engine 0x%02X\n", id & ~0x200);
            }
        }
        else {
            for (int level = 0; level < 16; level++) {
                for (float rpm : { 1000.0f, 4000.0f, 8000.0f }) {
                    rush2::engine2049::LayerMix mix[2];
                    if (!rush2::engine2049::layer_mix(level, rpm, 0.0f, mix)) continue;
                    for (const auto& m : mix) if (m.sound >= 0) ids.insert(m.sound);
                }
            }
            for (int id : ids) printf("[sweep] 2049 engine 0x%02X\n", id);
            std::vector<int16_t> pcm;
            uint32_t rate = 0;
            auto rom = rush2::wings::get_rom();
            printf("[sweep] 2049 source %s, effects from the %s player\n",
                   rom != nullptr && rom->is_dreamcast() ? "disc" : "N64",
                   rush2::audio2049::sfx_samples(0x3D, pcm, rate) ? "disc" : "N64");
        }
        fflush(stdout);
    }
    int count = g == Game::Rush2 ? rush2_effects : rush2::audio2049::sfx_count;
    // RUSH2_TEST_SFX_SWEEP_COUNT: only the first that many effects (a quick check).
    static const int limit = std::getenv("RUSH2_TEST_SFX_SWEEP_COUNT") ? std::atoi(std::getenv("RUSH2_TEST_SFX_SWEEP_COUNT")) : 0;
    if (limit > 0) count = std::min(count, limit);
    if (next > count || frame < next_frame) return;
    // The last effect off, and the music kept off (a menu may start it again).
    {
        recomp_context call = *ctx;
        if (g == Game::Rush2) snd_stop_and_set_sound_800620EC(rdram, &call);
        call = *ctx;
        music_stop_song_80061E10(rdram, &call);
    }
    rush2::track2049::stop_song_now();
    if (handle >= 0) rush2::audio2049::sfx_stop(handle);
    handle = -1;
    if (next == count) {
        printf("[sweep] done frame %llu\n", (unsigned long long)frame);
        fflush(stdout);
        next++;
        return;
    }
    if (g == Game::Rush2) {
        recomp_context call = *ctx;
        call.r4 = next;
        call.r5 = 0;
        snd_play_effect_80062264(rdram, &call);
    }
    else {
        handle = rush2::audio2049::sfx_start(next, 1.0f, 0.0f, 1.0f, -1.0f);
        if (handle >= 0 && ids.count(next)) rush2::audio2049::sfx_set_engine(handle);
    }
    printf("[sweep] %s 0x%02X frame %llu\n", g == Game::Rush2 ? "rush2" : "2049", next, (unsigned long long)frame);
    fflush(stdout);
    next++;
    next_frame = frame + uint64_t(step * rush2::sfx_sweep::dump_rate());
}
