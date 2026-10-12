// DualSense haptics: plays the rumble on a DualSense's two voice-coil actuators (include/dualsense_haptics.h).
//
// Over USB a DualSense is also a 4-channel 48 kHz sound device ("DualSense Wireless Controller"): channels 1 and 2 are
// its speaker and headphone jack, 3 and 4 the left and right actuators. Playing a waveform on channel 3 or 4 vibrates
// that side only, which is how PS5 games make their haptics. This opens that device with SDL's audio and fills the
// actuator channels with three voices whose loudness on each side follows set_levels: a low thump, a rough buzz (two
// tones a little apart, which beat against each other like tires chattering) and a grinding rasp (a low tone jumping in
// loudness, with noise). The speaker channels stay silent.
//
// A background thread does the opening and closing (device enumeration can take tens of milliseconds, too long for the
// VI thread that sets the levels), retrying every couple of seconds while a port wants haptics, so plugging the
// controller in mid-game picks it up.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "dualsense_haptics.h"

namespace {
    // ---- Tuning ----

    // The voices' pitches are the profile's (set_tones; defaults in include/control_profiles.h). The voice coils barely
    // move below about 40 Hz, and above about 250 Hz they start to sound more than they shake.
    std::atomic<double> thump_hz = 55.0;
    std::atomic<double> buzz_hz = 170.0;
    std::atomic<double> grind_hz = 95.0;

    // The thump: a low fundamental plus a little of its octave, which gives the coil some bite.
    constexpr float thump_octave_mix = 0.35f; // Loudness of the octave (2 * thump_hz) next to the fundamental.

    // The buzz: two tones that aren't harmonics of each other, so they beat into a rough texture: buzz_hz and
    // buzz_hz * buzz_rough_ratio.
    constexpr double buzz_rough_ratio = 233.0 / 170.0;
    constexpr float buzz_rough_mix = 0.6f; // Loudness of the second tone next to buzz_hz.

    // The grind: a low tone whose loudness jumps randomly (grind_jitter_hz times a second), plus filtered noise, so it
    // feels like metal dragging on concrete rather than a steady hum.
    constexpr double grind_jitter_hz = 30.0;
    constexpr float grind_noise_mix = 0.5f;    // Loudness of the noise next to the tone.
    constexpr float grind_noise_cutoff = 0.05f; // One-pole low-pass on the noise per sample (about 400 Hz at 48 kHz).

    // Loudness of a level of 1, from 0 to 1 of full scale, per voice. The voices together are clamped to 1.
    constexpr float thump_gain = 1.0f;
    constexpr float buzz_gain = 0.8f;
    constexpr float grind_gain = 0.9f;

    // How fast each side's loudness follows set_levels, in seconds to get most of the way there. Short enough to keep a
    // hit sharp, long enough not to click.
    constexpr double level_smoothing_s = 0.008;

    // Seconds between looks for the DualSense's sound device while one is wanted but not open.
    constexpr int scan_interval_s = 2;

    // Seconds a port can stop wanting haptics before the device is closed (a menu between races shouldn't reopen it).
    constexpr int idle_close_s = 10;

    constexpr int sample_rate = 48000;
    constexpr int channels = 4;
    constexpr int left_channel = 2;  // Channel 3.
    constexpr int right_channel = 3; // Channel 4.

    // Targets set by set_levels: thump left, thump right, buzz left, buzz right, grind left, grind right.
    constexpr int num_levels = 6;
    std::atomic<float> targets[num_levels] = {};
    std::atomic_bool wanted = false;
    std::atomic_bool device_open = false;
    std::once_flag thread_started;

    // Audio callback only.
    float levels[num_levels] = {};
    double thump_phase = 0.0;
    double buzz_phase = 0.0;
    double rough_phase = 0.0;
    double grind_phase = 0.0;
    uint32_t noise_state = 0x12345678;
    float noise_filtered = 0.0f;
    float grind_jitter = 1.0f;
    int jitter_samples_left = 0;
    float smoothing = 0.0f;

    float noise() { // -1 to 1.
        noise_state = noise_state * 1664525u + 1013904223u;
        return (float)(noise_state >> 8) / (float)(1 << 23) - 1.0f;
    }

    double advance(double& phase, double hz) {
        constexpr double two_pi = 6.283185307179586;
        double value = phase;
        phase += two_pi * hz / sample_rate;
        if (phase >= two_pi) {
            phase -= two_pi;
        }
        return value;
    }

    void fill(void*, Uint8* stream, int len) {
        float* out = reinterpret_cast<float*>(stream);
        int frames = len / (int)(sizeof(float) * channels);
        float want[num_levels];
        for (int v = 0; v < num_levels; v++) {
            want[v] = targets[v].load(std::memory_order_relaxed);
        }
        const double thump_tone = thump_hz.load(std::memory_order_relaxed);
        const double buzz_tone = buzz_hz.load(std::memory_order_relaxed);
        const double grind_tone = grind_hz.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; i++) {
            for (int v = 0; v < num_levels; v++) {
                levels[v] += (want[v] - levels[v]) * smoothing;
            }
            double t = advance(thump_phase, thump_tone);
            float thump = (float)((std::sin(t) + thump_octave_mix * std::sin(2.0 * t)) / (1.0 + thump_octave_mix));
            float buzz = (float)((std::sin(advance(buzz_phase, buzz_tone)) +
                buzz_rough_mix * std::sin(advance(rough_phase, buzz_tone * buzz_rough_ratio))) / (1.0 + buzz_rough_mix));
            if (--jitter_samples_left <= 0) {
                jitter_samples_left = (int)(sample_rate / grind_jitter_hz);
                grind_jitter = 0.55f + 0.45f * noise();
            }
            noise_filtered += (noise() - noise_filtered) * grind_noise_cutoff;
            float grind = ((float)std::sin(advance(grind_phase, grind_tone)) * grind_jitter +
                grind_noise_mix * noise_filtered * 4.0f) / (1.0f + grind_noise_mix);
            float* frame = out + i * channels;
            frame[0] = 0.0f;
            frame[1] = 0.0f;
            for (int side = 0; side < 2; side++) {
                frame[side == 0 ? left_channel : right_channel] = std::clamp(thump * levels[side] * thump_gain +
                    buzz * levels[2 + side] * buzz_gain + grind * levels[4 + side] * grind_gain, -1.0f, 1.0f);
            }
        }
    }

    bool is_dualsense_name(const char* name) {
        return name != nullptr && (strstr(name, "DualSense") != nullptr || strstr(name, "Wireless Controller") != nullptr);
    }

    // Opens the first DualSense sound device with 4 channels, or returns 0.
    SDL_AudioDeviceID open_device() {
        int count = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(i, 0);
            if (!is_dualsense_name(name)) {
                continue;
            }
            SDL_AudioSpec desired{};
            desired.freq = sample_rate;
            desired.format = AUDIO_F32SYS;
            desired.channels = channels;
            desired.samples = 256;
            desired.callback = fill;
            SDL_AudioSpec obtained{};
            // Let the channel count follow the device's, to find out whether it really has the actuator channels: a
            // stereo device would get the haptics mixed into its speaker.
            SDL_AudioDeviceID device = SDL_OpenAudioDevice(name, 0, &desired, &obtained, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
            if (device == 0) {
                fprintf(stderr, "[Haptics] Couldn't open %s: %s\n", name, SDL_GetError());
                continue;
            }
            if (obtained.channels != channels) {
                fprintf(stderr, "[Haptics] %s has %d channels, not 4; not using it\n", name, obtained.channels);
                SDL_CloseAudioDevice(device);
                continue;
            }
            printf("[Haptics] Using %s for DualSense haptics\n", name);
            return device;
        }
        return 0;
    }

    void run() {
        using clock = std::chrono::steady_clock;
        smoothing = (float)(1.0 - std::exp(-1.0 / (level_smoothing_s * sample_rate)));
        SDL_AudioDeviceID device = 0;
        clock::time_point last_wanted = clock::now();
        clock::time_point next_scan = clock::now();
        while (true) {
            auto now = clock::now();
            if (wanted) {
                last_wanted = now;
            }
            if (device != 0 && (SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_STOPPED ||
                                now - last_wanted > std::chrono::seconds(idle_close_s))) {
                device_open = false;
                SDL_CloseAudioDevice(device);
                device = 0;
                printf("[Haptics] Closed the DualSense's sound device\n");
            }
            if (device == 0 && wanted && now >= next_scan) {
                next_scan = now + std::chrono::seconds(scan_interval_s);
                device = open_device();
                if (device != 0) {
                    SDL_PauseAudioDevice(device, 0);
                    device_open = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void rush2::haptics::set_wanted(bool want) {
    wanted = want;
    if (want) {
        std::call_once(thread_started, [] { std::thread(run).detach(); });
    }
}

bool rush2::haptics::active() {
    return device_open;
}

void rush2::haptics::set_tones(float thump, float buzz, float grind) {
    thump_hz.store(thump, std::memory_order_relaxed);
    buzz_hz.store(buzz, std::memory_order_relaxed);
    grind_hz.store(grind, std::memory_order_relaxed);
}

void rush2::haptics::set_levels(const Levels& levels) {
    const float values[num_levels] = { levels.thump[0], levels.thump[1], levels.buzz[0], levels.buzz[1],
                                       levels.grind[0], levels.grind[1] };
    for (int v = 0; v < num_levels; v++) {
        targets[v].store(values[v], std::memory_order_relaxed);
    }
}
