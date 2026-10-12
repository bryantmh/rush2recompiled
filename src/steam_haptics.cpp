// Steam Controller (2026) haptics as tones (include/steam_haptics.h).
//
// SDL drives the controller's actuators with output report 0x80 (ID_OUT_REPORT_HAPTIC_RUMBLE): a "speed" per side, as
// for rumble motors, so a crash, a slide and a wall scrape on one side all became the same buzz at different
// strengths. The controller also takes output report 0x83 (ID_OUT_REPORT_HAPTIC_LFO_TONE, SDL's
// steam/controller_structs.h, packed):
//   [0] 0x83  [1] actuator  [2] gain_db (s8, 0 = full, -128 = silent)  [3-4] frequency Hz  [5-6] duration_ms
//   (0x7FFF holds the tone until the next report)  [7-8] LFO frequency Hz  [9] LFO depth %
// The actuator is an index: 0 and 1 the trackpads, 3 and 4 the left and right grips. Tested by others on the hardware
// (hifihedgehog/PadForge HapticToneEncoder.cs, from SteamHapticsSinger): positive gain clips; a silent tone is gain
// -128 with duration 0x8000; four actuators at once garble over USB while two play clean; and over USB a 0x80 rumble
// report garbles the tones after it, so SDL's rumble is left at zero while tones play (src/input.cpp).
//
// Each grip plays one tone at a time, so it plays the strongest of the side's three voices, with the DualSense's
// pitches (the profile's): the thump plain, the buzz with an LFO at the beat of the DualSense's two buzz tones (233 Hz
// against 170 Hz), the grind with a faster, shallower LFO standing in for its random jumps and noise. A tone is only
// sent again when its voice changes or its gain moves by min_gain_step_db, and no more often than min_send_ms a side
// (the puck relays one report per radio poll).
//
// The device is opened on its own (Windows shares HID devices), not through SDL, whose effect call only sends feature
// reports. A background thread does the opening and the writes, which can block.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#endif

#include "steam_haptics.h"

namespace {
    constexpr unsigned vendor_valve = 0x28DE;
    constexpr uint8_t report_lfo_tone = 0x83;
    constexpr uint8_t grip_actuators[2] = { 3, 4 }; // Left, right.
    constexpr int8_t gain_silent = -128;
    constexpr float min_gain_db = -40.0f;      // Quieter than this is silent.
    constexpr float silent_level = 0.01f;      // A voice under this level is off.
    constexpr float min_gain_step_db = 1.5f;   // A smaller change of gain isn't sent.
    constexpr int min_send_ms = 30;            // Per side.
    constexpr int refresh_ms = 2000;           // A held tone is sent again this often, in case a report was lost.

    // The buzz's LFO: the beat between the DualSense's two buzz tones (233 Hz against 170 Hz), as deep as its second
    // tone is loud next to the first (dualsense_haptics.cpp: buzz_rough_mix).
    constexpr float buzz_lfo_ratio = 233.0f / 170.0f - 1.0f;
    constexpr uint8_t buzz_lfo_depth = 60;
    // The grind's LFO: as often as the DualSense's grind jumps (grind_jitter_hz) and about as deep.
    constexpr uint16_t grind_lfo_hz = 30;
    constexpr uint8_t grind_lfo_depth = 45;

    enum class Voice { None, Thump, Buzz, Grind };

    struct Tone {
        Voice voice = Voice::None;
        int8_t gain_db = gain_silent;
        uint16_t frequency = 0;
        uint16_t lfo_hz = 0;
        uint8_t lfo_depth = 0;
    };

    std::mutex mutex;
    std::string wanted_path;               // Under mutex.
    rush2::haptics::Levels wanted_levels;  // Under mutex.
    float tone_hz[3] = { 55.0f, 170.0f, 95.0f }; // Thump, buzz, grind; under mutex.
    std::atomic_bool device_active = false;
    std::once_flag thread_started;

    int8_t gain_of(float level) {
        float db = 20.0f * std::log10(std::max(level, 1e-6f));
        return db < min_gain_db ? gain_silent : (int8_t)std::lround(std::min(db, 0.0f));
    }

    // The side's tone: its strongest voice.
    Tone tone_for(const rush2::haptics::Levels& levels, int side, const float hz[3]) {
        float thump = levels.thump[side], buzz = levels.buzz[side], grind = levels.grind[side];
        Tone tone;
        float level = std::max({ thump, buzz, grind });
        if (level < silent_level) {
            return tone;
        }
        tone.gain_db = gain_of(level);
        if (tone.gain_db == gain_silent) {
            return tone;
        }
        if (thump >= buzz && thump >= grind) {
            tone.voice = Voice::Thump;
            tone.frequency = (uint16_t)hz[0];
        }
        else if (grind >= buzz) {
            tone.voice = Voice::Grind;
            tone.frequency = (uint16_t)hz[2];
            tone.lfo_hz = grind_lfo_hz;
            tone.lfo_depth = grind_lfo_depth;
        }
        else {
            tone.voice = Voice::Buzz;
            tone.frequency = (uint16_t)hz[1];
            tone.lfo_hz = (uint16_t)std::lround(hz[1] * buzz_lfo_ratio);
            tone.lfo_depth = buzz_lfo_depth;
        }
        return tone;
    }

#ifdef _WIN32
    struct Device {
        HANDLE handle = INVALID_HANDLE_VALUE;
        size_t report_size = 0; // The output report length writes are padded to (as hidapi does).

        bool open(const std::string& path) {
            handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                return false;
            }
            PHIDP_PREPARSED_DATA data = nullptr;
            HIDP_CAPS caps{};
            if (HidD_GetPreparsedData(handle, &data)) {
                HidP_GetCaps(data, &caps);
                HidD_FreePreparsedData(data);
            }
            report_size = caps.OutputReportByteLength;
            if (report_size < 10) {
                close();
                return false;
            }
            return true;
        }

        void close() {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            handle = INVALID_HANDLE_VALUE;
        }

        bool send(uint8_t actuator, const Tone& tone) {
            std::vector<uint8_t> report(report_size, 0);
            report[0] = report_lfo_tone;
            report[1] = actuator;
            report[2] = (uint8_t)tone.gain_db;
            uint16_t duration = tone.voice == Voice::None ? 0x8000 : 0x7FFF;
            report[3] = (uint8_t)(tone.frequency & 0xFF);
            report[4] = (uint8_t)(tone.frequency >> 8);
            report[5] = (uint8_t)(duration & 0xFF);
            report[6] = (uint8_t)(duration >> 8);
            report[7] = (uint8_t)(tone.lfo_hz & 0xFF);
            report[8] = (uint8_t)(tone.lfo_hz >> 8);
            report[9] = tone.lfo_depth;
            DWORD written = 0;
            return WriteFile(handle, report.data(), (DWORD)report.size(), &written, nullptr) != 0;
        }
    };

    void run() {
        using clock = std::chrono::steady_clock;
        Device device;
        std::string open_path;
        Tone sent[2];
        clock::time_point sent_at[2] = {};
        while (true) {
            std::string path;
            rush2::haptics::Levels levels;
            float hz[3];
            {
                std::lock_guard lock{ mutex };
                path = wanted_path;
                levels = wanted_levels;
                std::copy(tone_hz, tone_hz + 3, hz);
            }
            if (path != open_path) {
                if (device.handle != INVALID_HANDLE_VALUE) {
                    for (int side = 0; side < 2; side++) {
                        device.send(grip_actuators[side], Tone{});
                    }
                    device.close();
                    printf("[Haptics] Closed the Steam Controller's haptics\n");
                }
                device_active = false;
                open_path = path;
                sent[0] = sent[1] = Tone{};
                if (!path.empty()) {
                    if (device.open(path)) {
                        device_active = true;
                        printf("[Haptics] Using the Steam Controller's haptics (%s)\n", path.c_str());
                    }
                    else {
                        fprintf(stderr, "[Haptics] Couldn't open the Steam Controller for haptics (%s, error %lu)\n",
                            path.c_str(), GetLastError());
                    }
                }
            }
            if (device_active) {
                auto now = clock::now();
                for (int side = 0; side < 2; side++) {
                    Tone tone = tone_for(levels, side, hz);
                    const Tone& last = sent[side];
                    bool changed = tone.voice != last.voice || tone.frequency != last.frequency ||
                        tone.lfo_hz != last.lfo_hz || std::abs(tone.gain_db - last.gain_db) >= min_gain_step_db;
                    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_at[side]).count();
                    bool stale = tone.voice != Voice::None && since >= refresh_ms;
                    if ((changed && since >= min_send_ms) || stale) {
                        if (!device.send(grip_actuators[side], tone)) {
                            fprintf(stderr, "[Haptics] Steam Controller write failed (error %lu)\n", GetLastError());
                            device.close();
                            device_active = false;
                            break;
                        }
                        sent[side] = tone;
                        sent_at[side] = now;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(open_path.empty() ? 100 : 5));
        }
    }
#endif
}

bool rush2::steam_haptics::is_supported(unsigned vendor, unsigned product) {
#ifdef _WIN32
    return vendor == vendor_valve && product >= 0x1302 && product <= 0x1305;
#else
    (void)vendor;
    (void)product;
    return false;
#endif
}

void rush2::steam_haptics::set_device(const std::string& path) {
    {
        std::lock_guard lock{ mutex };
        if (path == wanted_path) {
            return;
        }
        wanted_path = path;
        if (path.empty()) {
            wanted_levels = {};
        }
    }
#ifdef _WIN32
    if (!path.empty()) {
        std::call_once(thread_started, [] { std::thread(run).detach(); });
    }
#endif
}

bool rush2::steam_haptics::active() {
    return device_active;
}

void rush2::steam_haptics::set_levels(const rush2::haptics::Levels& levels, float thump_hz, float buzz_hz, float grind_hz) {
    std::lock_guard lock{ mutex };
    wanted_levels = levels;
    tone_hz[0] = thump_hz;
    tone_hz[1] = buzz_hz;
    tone_hz[2] = grind_hz;
}
