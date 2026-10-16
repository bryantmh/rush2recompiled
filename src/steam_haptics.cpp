// Steam Controller (2026) and Steam Deck haptics as tones (include/steam_haptics.h).
//
// SDL drives the Steam Controller's actuators with output report 0x80 (ID_OUT_REPORT_HAPTIC_RUMBLE) and the Deck's with
// feature report 0xEB (ID_TRIGGER_RUMBLE_CMD): a "speed" per side, as for rumble motors, so a crash, a slide and a
// wall scrape on one side all became the same buzz at different strengths. Both also play tones.
//
// Steam Controller: output report 0x83 (ID_OUT_REPORT_HAPTIC_LFO_TONE, SDL's steam/controller_structs.h, packed):
//   [0] 0x83  [1] actuator  [2] gain_db (s8, 0 = full, -128 = silent)  [3-4] frequency Hz  [5-6] duration_ms
//   (0x7FFF holds the tone until the next report)  [7-8] LFO frequency Hz  [9] LFO depth %
// The actuator is an index: 0 and 1 the left and right trackpads, 3 and 4 the left and right grips. Tested by others on
// the hardware (hifihedgehog/PadForge HapticToneEncoder.cs, from SteamHapticsSinger): positive gain clips; a silent
// tone is gain -128 with duration 0x8000; four actuators at once garble over USB and through the puck while two play
// clean; and over USB a 0x80 rumble report garbles the tones after it, so SDL's rumble is left at zero while tones play
// (src/input.cpp). So wired and through the puck each grip plays the strongest of its side's three voices, and over
// Bluetooth the trackpads take the screech's buzz while the grips play the thump or the grind.
//
// Steam Deck: feature report 0xEA (ID_TRIGGER_HAPTIC_CMD, MsgTriggerHaptic in SDL's steam/controller_structs.h,
// packed), sent the way SDL_hidapi_steamdeck.c sends its rumble: 64 bytes after the report ID 0, starting with a
// {type, length} header:
//   [0] side (1 left, 2 right)  [1] command (0 off, 3 tone)  [2] ui_intensity (0 default)  [3] dB gain (s8)
//   [4-5] frequency Hz  [6-7] duration ms (s16, negative holds it)  [8-9] noise intensity  [10-11] LFO frequency Hz
//   [12] LFO depth %  [13] random tone gain  [14] script ID  [15-16] sweep start Hz  [17-18] sweep end Hz
// Each side's pad plays the strongest of its three voices.
//
// The voices are the DualSense's pitches (the profile's): the thump plain, the buzz with an LFO at the beat of the
// DualSense's two buzz tones (233 Hz against 170 Hz), the grind with a faster, shallower LFO standing in for its random
// jumps and noise. A tone is only sent again when its voice changes or its gain moves by min_gain_step_db, and no more
// often than min_send_ms an actuator (the puck relays one report per radio poll).
//
// Each controller is opened on its own (Windows shares HID devices), not through SDL, whose effect call only sends
// feature reports to the Steam Controller and none to the Deck. A thread per controller does the opening and the
// writes, which can block.
//
// On Linux the reports go to the controller's hidraw node (write() for output reports, HIDIOCSFEATURE for feature
// reports; opening it needs the udev rules Steam installs). SDL's path is that node when its HIDAPI driver has the
// controller. Otherwise it's an evdev node, whose USB device holds the node, or Steam Input's virtual device, which
// takes a node with the controller's IDs (SDL reports the real ones) that no other player has. Of a controller's
// nodes, the Deck's is interface 2 (as SDL_hidapi_steamdeck.c picks) and the Steam Controller's the one with tone
// reports (src/linux_devices.cpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#elif defined(__linux__)
#include <cerrno>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include "linux_devices.h"
#endif

#include "steam_haptics.h"

#if defined(_WIN32) || defined(__linux__)
namespace {
    constexpr unsigned vendor_valve = 0x28DE;
    constexpr unsigned product_triton_wired = 0x1302;
    constexpr unsigned product_triton_bluetooth = 0x1303;
    constexpr unsigned product_triton_puck = 0x1304;
    constexpr unsigned product_triton_puck_2 = 0x1305;
    constexpr unsigned product_steam_deck = 0x1205;

    constexpr uint8_t report_lfo_tone = 0x83;
    constexpr uint8_t deck_haptic_command = 0xEA;
    constexpr uint8_t deck_command_off = 0;
    constexpr uint8_t deck_command_tone = 3;
    constexpr size_t deck_feature_bytes = 65; // Report ID 0, then 64 bytes.
    constexpr size_t deck_haptic_bytes = 19;  // sizeof(MsgTriggerHaptic).

    constexpr int8_t gain_silent = -128;
    constexpr float min_gain_db = -40.0f;      // Quieter than this is silent.
    constexpr float silent_level = 0.01f;      // A voice under this level is off.
    constexpr float min_gain_step_db = 1.5f;   // A smaller change of gain isn't sent.
    constexpr int min_send_ms = 30;            // Per actuator.
    constexpr int refresh_ms = 2000;           // A held tone is sent again this often, in case a report was lost.
    constexpr int retry_s = 2;                 // Between tries to open a controller that wouldn't.
    constexpr int idle_close_s = 10;           // A controller not wanted this long is silenced and closed.

    // The buzz's LFO: the beat between the DualSense's two buzz tones (233 Hz against 170 Hz), as deep as its second
    // tone is loud next to the first (dualsense_haptics.cpp: buzz_rough_mix).
    constexpr float buzz_lfo_ratio = 233.0f / 170.0f - 1.0f;
    constexpr uint8_t buzz_lfo_depth = 60;
    // The grind's LFO: as often as the DualSense's grind jumps (grind_jitter_hz) and about as deep.
    constexpr uint16_t grind_lfo_hz = 30;
    constexpr uint8_t grind_lfo_depth = 45;

    enum class Voice { None, Thump, Buzz, Grind };
    // Which voices an actuator plays (the strongest of them).
    constexpr int voice_thump = 1, voice_buzz = 2, voice_grind = 4, all_voices = 7;

    struct Tone {
        Voice voice = Voice::None;
        int8_t gain_db = gain_silent;
        uint16_t frequency = 0;
        uint16_t lfo_hz = 0;
        uint8_t lfo_depth = 0;
    };

    // One actuator: its index in the reports (a Steam Controller actuator, or a Deck side 1 or 2), the side whose
    // levels it plays and which voices.
    struct Actuator {
        uint8_t index;
        int side;
        int voices;
    };

    int8_t gain_of(float level) {
        float db = 20.0f * std::log10(std::max(level, 1e-6f));
        return db < min_gain_db ? gain_silent : (int8_t)std::lround(std::min(db, 0.0f));
    }

    // The actuator's tone: the strongest of its voices.
    Tone tone_for(const rush2::haptics::Levels& levels, const Actuator& actuator, const float hz[3]) {
        float thump = actuator.voices & voice_thump ? levels.thump[actuator.side] : 0.0f;
        float buzz = actuator.voices & voice_buzz ? levels.buzz[actuator.side] : 0.0f;
        float grind = actuator.voices & voice_grind ? levels.grind[actuator.side] : 0.0f;
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
    unsigned long last_error() { return GetLastError(); }
#else
    unsigned long last_error() { return (unsigned long)errno; }

    // hidraw nodes a player has open, so a controller behind Steam Input doesn't take another player's.
    std::mutex nodes_mutex;
    std::set<std::string> nodes_in_use;
#endif

    // One controller: its levels and the thread sending them.
    struct Player {
        std::string path;
        unsigned product = 0;
        std::atomic_bool stop = false;
        std::atomic_bool open = false; // Opened and a report went through.
        std::chrono::steady_clock::time_point last_wanted; // Under players_mutex.
        std::thread thread;

        std::mutex levels_mutex;
        rush2::haptics::Levels levels;                   // Under levels_mutex.
        float tone_hz[3] = { 55.0f, 170.0f, 95.0f };     // Thump, buzz, grind; under levels_mutex.

#ifdef _WIN32
        HANDLE handle = INVALID_HANDLE_VALUE;
#else
        int fd = -1;
        std::string node; // The hidraw node open, in nodes_in_use.
#endif
        size_t report_size = 0; // The output report length writes are padded to (as hidapi does).

        bool is_deck() const { return product == product_steam_deck; }

        std::vector<Actuator> actuators() const {
            if (is_deck()) {
                return { { 1, 0, all_voices }, { 2, 1, all_voices } };
            }
            if (product == product_triton_bluetooth) {
                return { { 3, 0, voice_thump | voice_grind }, { 4, 1, voice_thump | voice_grind },
                         { 0, 0, voice_buzz }, { 1, 1, voice_buzz } };
            }
            return { { 3, 0, all_voices }, { 4, 1, all_voices } };
        }

#ifdef _WIN32
        bool open_device() {
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
            report_size = is_deck() ? std::max<size_t>(caps.FeatureReportByteLength, deck_feature_bytes)
                                    : caps.OutputReportByteLength;
            if (!is_deck() && report_size < 10) {
                close_device();
                return false;
            }
            return true;
        }

        void close_device() {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            handle = INVALID_HANDLE_VALUE;
        }

        bool set_feature(std::vector<uint8_t>& report) {
            return HidD_SetFeature(handle, report.data(), (ULONG)report.size()) != 0;
        }

        bool write_output(std::vector<uint8_t>& report) {
            DWORD written = 0;
            return WriteFile(handle, report.data(), (DWORD)report.size(), &written, nullptr) != 0;
        }

#else
        // Takes the hidraw node SDL's path stands for into nodes_in_use, or empty.
        std::string claim_node() {
            bool hidraw = path.rfind("/dev/hidraw", 0) == 0;
            std::string usb = hidraw ? std::string{} : rush2::linux_devices::usb_device_of(path);
            std::lock_guard lock{ nodes_mutex };
            if (hidraw) {
                nodes_in_use.insert(path);
                return path;
            }
            for (const auto& h : rush2::linux_devices::hidraw_devices()) {
                if (h.vendor != vendor_valve || h.product != product ||
                    (usb.empty() ? nodes_in_use.count(h.node) != 0 : h.usb_device != usb)) {
                    continue;
                }
                bool reports = is_deck() ? h.interface == 2
                    : rush2::linux_devices::report_bytes(h.node, rush2::linux_devices::ReportKind::Output,
                                                         report_lfo_tone) >= 10;
                if (reports) {
                    nodes_in_use.insert(h.node);
                    return h.node;
                }
            }
            return {};
        }

        bool open_device() {
            node = claim_node();
            if (node.empty()) {
                errno = ENODEV;
                return false;
            }
            fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                int error = errno;
                close_device();
                errno = error;
                return false;
            }
            using rush2::linux_devices::ReportKind;
            report_size = is_deck()
                ? std::max(rush2::linux_devices::report_bytes(node, ReportKind::Feature, 0), deck_feature_bytes)
                : rush2::linux_devices::report_bytes(node, ReportKind::Output, report_lfo_tone);
            if (!is_deck() && report_size < 10) {
                close_device();
                errno = ENOTSUP;
                return false;
            }
            return true;
        }

        void close_device() {
            if (fd >= 0) {
                ::close(fd);
            }
            fd = -1;
            if (!node.empty()) {
                std::lock_guard lock{ nodes_mutex };
                nodes_in_use.erase(node);
                node.clear();
            }
        }

        bool set_feature(std::vector<uint8_t>& report) {
            return ioctl(fd, HIDIOCSFEATURE(report.size()), report.data()) >= 0;
        }

        bool write_output(std::vector<uint8_t>& report) {
            return ::write(fd, report.data(), report.size()) == (ssize_t)report.size();
        }
#endif

        bool send(const Actuator& actuator, const Tone& tone) {
            std::vector<uint8_t> report(report_size, 0);
            bool silent = tone.voice == Voice::None;
            if (is_deck()) {
                report[1] = deck_haptic_command;
                report[2] = (uint8_t)deck_haptic_bytes;
                uint8_t* msg = report.data() + 3;
                int16_t duration = -1; // Held until the next report.
                msg[0] = actuator.index;
                msg[1] = silent ? deck_command_off : deck_command_tone;
                msg[3] = (uint8_t)tone.gain_db;
                msg[4] = (uint8_t)(tone.frequency & 0xFF);
                msg[5] = (uint8_t)(tone.frequency >> 8);
                msg[6] = (uint8_t)(duration & 0xFF);
                msg[7] = (uint8_t)((uint16_t)duration >> 8);
                msg[10] = (uint8_t)(tone.lfo_hz & 0xFF);
                msg[11] = (uint8_t)(tone.lfo_hz >> 8);
                msg[12] = tone.lfo_depth;
                return set_feature(report);
            }
            uint16_t duration = silent ? 0x8000 : 0x7FFF;
            report[0] = report_lfo_tone;
            report[1] = actuator.index;
            report[2] = (uint8_t)tone.gain_db;
            report[3] = (uint8_t)(tone.frequency & 0xFF);
            report[4] = (uint8_t)(tone.frequency >> 8);
            report[5] = (uint8_t)(duration & 0xFF);
            report[6] = (uint8_t)(duration >> 8);
            report[7] = (uint8_t)(tone.lfo_hz & 0xFF);
            report[8] = (uint8_t)(tone.lfo_hz >> 8);
            report[9] = tone.lfo_depth;
            return write_output(report);
        }

        // Silences every actuator; false if a report didn't go through.
        bool silence(const std::vector<Actuator>& list) {
            bool ok = true;
            for (const Actuator& actuator : list) {
                ok &= send(actuator, Tone{});
            }
            return ok;
        }

        void run() {
            using clock = std::chrono::steady_clock;
            const std::vector<Actuator> list = actuators();
            std::vector<Tone> sent(list.size());
            std::vector<clock::time_point> sent_at(list.size());
            while (!stop) {
                if (!open) {
                    // Only counted as open once the silence went through, so a device that won't take the reports
                    // keeps SDL's rumble.
                    if (open_device() && silence(list)) {
                        open = true;
                        std::fill(sent.begin(), sent.end(), Tone{});
                        printf("[Haptics] Playing Steam %s tones on %s\n", is_deck() ? "Deck" : "Controller",
                            path.c_str());
                    }
                    else {
                        fprintf(stderr, "[Haptics] Couldn't play tones on %s (error %lu)\n", path.c_str(),
                            last_error());
                        close_device();
                        for (int i = 0; i < retry_s * 10 && !stop; i++) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        continue;
                    }
                }
                rush2::haptics::Levels levels;
                float hz[3];
                {
                    std::lock_guard lock{ levels_mutex };
                    levels = this->levels;
                    std::copy(tone_hz, tone_hz + 3, hz);
                }
                auto now = clock::now();
                for (size_t i = 0; i < list.size(); i++) {
                    Tone tone = tone_for(levels, list[i], hz);
                    const Tone& last = sent[i];
                    bool changed = tone.voice != last.voice || tone.frequency != last.frequency ||
                        tone.lfo_hz != last.lfo_hz || std::abs(tone.gain_db - last.gain_db) >= min_gain_step_db;
                    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_at[i]).count();
                    bool stale = tone.voice != Voice::None && since >= refresh_ms;
                    if ((changed && since >= min_send_ms) || stale) {
                        if (!send(list[i], tone)) {
                            fprintf(stderr, "[Haptics] Tone write to %s failed (error %lu)\n", path.c_str(),
                                last_error());
                            open = false;
                            close_device();
                            break;
                        }
                        sent[i] = tone;
                        sent_at[i] = now;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (open) {
                silence(list);
                printf("[Haptics] Stopped the tones on %s\n", path.c_str());
            }
            open = false;
            close_device();
        }
    };

    std::mutex players_mutex;
    // By path; under players_mutex. Never destroyed: a player's thread runs as long as the game, and destroying it at
    // exit would abort the process (a joinable std::thread) and write a crash dump, which held up closing.
    std::map<std::string, std::unique_ptr<Player>>& players = *new std::map<std::string, std::unique_ptr<Player>>;
    std::once_flag manager_started;

    // Stops the players no longer wanted (joining their threads, which can take a moment, off the VI thread).
    void manage() {
        while (true) {
            std::vector<std::unique_ptr<Player>> closing;
            {
                std::lock_guard lock{ players_mutex };
                auto now = std::chrono::steady_clock::now();
                for (auto it = players.begin(); it != players.end();) {
                    if (now - it->second->last_wanted > std::chrono::seconds(idle_close_s)) {
                        it->second->stop = true;
                        closing.push_back(std::move(it->second));
                        it = players.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            for (auto& player : closing) {
                player->thread.join();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

bool rush2::steam_haptics::is_supported(unsigned vendor, unsigned product) {
    return vendor == vendor_valve &&
        (product == product_triton_wired || product == product_triton_bluetooth || product == product_triton_puck ||
         product == product_triton_puck_2 || product == product_steam_deck);
}

void rush2::steam_haptics::set_controllers(const std::vector<Controller>& controllers) {
    if (controllers.empty()) {
        return;
    }
    std::call_once(manager_started, [] { std::thread(manage).detach(); });
    std::lock_guard lock{ players_mutex };
    auto now = std::chrono::steady_clock::now();
    for (const Controller& c : controllers) {
        auto& player = players[c.path];
        if (player == nullptr) {
            player = std::make_unique<Player>();
            player->path = c.path;
            player->product = c.product;
            player->thread = std::thread([p = player.get()] { p->run(); });
        }
        player->last_wanted = now;
    }
}

bool rush2::steam_haptics::active(const std::string& path) {
    std::lock_guard lock{ players_mutex };
    auto it = players.find(path);
    return it != players.end() && it->second->open;
}

void rush2::steam_haptics::set_levels(const std::string& path, const rush2::haptics::Levels& levels, float thump_hz,
                                      float buzz_hz, float grind_hz) {
    std::lock_guard lock{ players_mutex };
    auto it = players.find(path);
    if (it == players.end()) {
        return;
    }
    Player& player = *it->second;
    std::lock_guard levels_lock{ player.levels_mutex };
    player.levels = levels;
    player.tone_hz[0] = thump_hz;
    player.tone_hz[1] = buzz_hz;
    player.tone_hz[2] = grind_hz;
}
#else // Other systems: none.
bool rush2::steam_haptics::is_supported(unsigned, unsigned) { return false; }
void rush2::steam_haptics::set_controllers(const std::vector<Controller>&) {}
bool rush2::steam_haptics::active(const std::string&) { return false; }
void rush2::steam_haptics::set_levels(const std::string&, const rush2::haptics::Levels&, float, float, float) {}
#endif
