// Rush 2: Recompiled entry point. Structure follows BanjoRecomp's main.cpp (RecompFrontend integration).

#include <array>
#include <chrono>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hitch_profiler.h"
#include "nfd.h"
#include "texture_census.h"
#include "texture_derive.h"
#include "render/rt64_raster_shader_cache.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "elements/ui_image.h"
#include "util/file.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/rsp.hpp"

#include "rush2.h"
#include "arcade2049.h"
#include "light_bar.h"
#include "online.h"
#include "data_files.h"
#include "wings.h"
#include "track2049.h"
#include "track_mod_runtime.h"
#include "prewarm.h"
#include "setup.h"
#include "car_engines.h"
#include "steam_input_filter.h"
#include "linux_window.h"
#include "sim_run.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include "SDL_syswm.h"
#endif

// The latest git tag, set by CMakeLists.txt.
const std::string version_string = RUSH2_VERSION;

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Xbox controllers otherwise open through SDL's RawInput driver, which got only the triggers (from XInput) and no
    // buttons from an Xbox Elite controller on the Xbox Wireless Adapter; XInput reports all of them.
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) > 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());

    return {};
}

static bool script_uses_controller[4] = {}; // Set by an --input-script with P2-P4 presses.

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (rush2::input::is_port_connected(controller_num) ||
        (controller_num >= 0 && controller_num < 4 && script_uses_controller[controller_num])) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

SDL_Window* window;

// Window icon: raw 128x128 RGBA pixels written by tools/build_icons.py.
[[maybe_unused]] static void set_window_icon(SDL_Window* window) {
    constexpr int icon_size = 128;
    std::ifstream file{ recompui::file::get_program_path() / "assets" / "icon_128.rgba", std::ios::binary };
    std::vector<char> pixels(icon_size * icon_size * 4);
    if (!file.read(pixels.data(), (std::streamsize)pixels.size())) {
        return;
    }
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), icon_size, icon_size, 32, icon_size * 4,
                                                           SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;
    if (rush2::sim::active()) {
        // A simulation run shows nothing (src/sim_run.cpp), unless RUSH2_SIM_SHOW is set (test aid: tools/test_race.py
        // --shots captures the window); then it's shown without taking the focus.
        if (std::getenv("RUSH2_SIM_SHOW") == nullptr) {
            flags |= SDL_WINDOW_HIDDEN;
        } else {
            SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        }
    }

#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow(rush2::program_name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900, flags);

    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }

#if !defined(_WIN32)
    // On Windows, SDL takes the window icon from the executable's icon resource (icons/app.rc).
    set_window_icon(window);
#endif

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#if defined(_WIN32)
    rush2::steam_input_filter::install(wmInfo.info.win.window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__)
#if defined(__linux__)
    // The file pickers stay in front of the game (src/linux_window.cpp).
    rush2::linux_window::set_dialog_parent(wmInfo.subsystem == SDL_SYSWM_X11 ? (unsigned long)wmInfo.info.x11.window : 0);
#endif
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window,  SDL_Metal_GetLayer(view) };
#else
    static_assert(false && "Unimplemented");
#endif
}

// recompui ignores mouse motion while a controller is the active input, so opening the menu with Escape left the
// cursor hidden until a click. When Escape is about to open the menu, queue an X2 button press behind it: recompui
// treats any button press as switching to the mouse, and RmlUi only clicks on the left button, so nothing is pressed.
static void show_cursor_on_escape_menu() {
    if (!ultramodern::is_game_started() || recompui::is_context_shown(recompui::config::get_config_context_id())) {
        return;
    }
    SDL_PumpEvents();
    SDL_Event events[32];
    int count = SDL_PeepEvents(events, 32, SDL_PEEKEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
    for (int i = 0; i < count; i++) {
        if (events[i].key.keysym.scancode == SDL_SCANCODE_ESCAPE && !events[i].key.repeat) {
            SDL_Event button{};
            button.button.type = SDL_MOUSEBUTTONDOWN;
            button.button.windowID = events[i].key.windowID;
            button.button.button = SDL_BUTTON_X2;
            button.button.state = SDL_PRESSED;
            SDL_PushEvent(&button);
            button.button.type = SDL_MOUSEBUTTONUP;
            button.button.state = SDL_RELEASED;
            SDL_PushEvent(&button);
            return;
        }
    }
}

void update_gfx(void*) {
    std::lock_guard lock{ rush2::input::sdl_device_mutex() };
    show_cursor_on_escape_menu();
    recompinput::handle_events();
}

// Audio

static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;

// Samples per channel per second.
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;

// Number of frames to duplicate for fixing interpolation at the start and end of a chunk.
constexpr uint32_t duplicated_input_frames = 4;
// The number of output frames to skip for playback (to avoid playing duplicate inputs twice).
static uint32_t discarded_output_frames;

constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (rush2::sim::active()) {
        return; // a simulation run is silent, and in turbo makes audio far faster than it plays
    }
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }

    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }

    // Convert to float and swap channels to undo the address xor from endianness handling.
    float cur_main_volume = static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f;
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (0.5f / 32768.0f) * cur_main_volume;
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (0.5f / 32768.0f) * cur_main_volume;
    }
    rush2::wings::mix_sound(&swap_buffer[duplicated_input_frames * input_channels], sample_count, sample_rate, (0.5f / 32768.0f) * cur_main_volume);
    rush2::track2049::mix_audio(&swap_buffer[duplicated_input_frames * input_channels], sample_count, sample_rate, 0.5f * cur_main_volume);
    rush2::car_engines::mix(&swap_buffer[duplicated_input_frames * input_channels], sample_count, sample_rate, (0.5f / 32768.0f) * cur_main_volume);

    if (sample_count <= duplicated_input_frames * input_channels) {
        return;
    }

    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }

    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);

    if (SDL_ConvertAudio(&audio_convert) < 0) {
        printf("Error using SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error using SDL audio converter");
    }

    // The queue holds converted audio, so measure it at the output rate (the game runs at 22050 Hz).
    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / (output_channels * sizeof(float)) * 1000000 / output_sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    // Drop samples if too much latency has built up.
    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }

    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
}

size_t get_frames_remaining() {
    // Rush 2 sizes each audio frame from osAiGetLength and lets the queue run down to near empty before topping it
    // up. At 1 VI the SDL queue regularly ran dry (silence padding = small random pops in the music); 2 VIs keeps
    // it at 20 ms or more.
    constexpr float buffer_offset_frames = 2.0f;
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

void update_audio_converter() {
    if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
        printf("Error creating SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error creating SDL audio converter");
    }
    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

void set_frequency(uint32_t freq) {
    sample_rate = freq;
    update_audio_converter();
}

bool reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{
        .freq = (int)output_freq,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0,
        .samples = 0x100,
        .padding = 0,
        .size = 0,
        .callback = nullptr,
        .userdata = nullptr
    };

    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        std::string audio_error = std::string("No audio device could be found. Please make sure an audio device is available.\nError opening audio device: ") + std::string(SDL_GetError());
        recompui::message_box(audio_error.c_str());
        return false;
    }
    SDL_PauseAudioDevice(audio_device, 0);

    output_sample_rate = output_freq;
    update_audio_converter();
    return true;
}

// RSP

extern RspUcodeFunc aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return aspMain;
    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

// Game registration

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0xEE71D0C9CA3A66DFULL,
        .internal_name = "RUSH 2",
        .display_name = "Rush 2: Extreme Racing USA",
        .game_id = u8"rush2.n64.us",
        .mod_game_id = "rush2",
        .save_type = recomp::SaveType::None,
        .is_enabled = true,
        .has_compressed_code = false,
        .entrypoint_address = (gpr)(int32_t)0x80000400,
        .entrypoint = recomp_entrypoint,
    },
};

// Imports a ROM into the config folder if one isn't stored yet, so the launcher can start the game
// right away. Candidates are command line arguments that aren't flags, then any .z64/.n64/.v64 file
// next to the executable or in its parent directory. Otherwise the launcher asks for one.
static void auto_import_rom(int argc, char** argv) {
    std::u8string game_id = supported_games[0].game_id;
    recomp::check_all_stored_roms();
    if (recomp::is_rom_valid(game_id)) {
        return;
    }
    // Test aid: RUSH2_TEST_FIRST_BOOT=1 in a simulation run's own data folder (src/sim_run.cpp) leaves the ROM
    // unimported, so the launcher shows Locate ROM as on a first boot without touching the player's data.
    if (!rush2::sim::data_dir().empty() && getenv("RUSH2_TEST_FIRST_BOOT") != nullptr) {
        return;
    }

    std::vector<std::filesystem::path> candidates;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input-script") == 0) {
            i++;
        }
        else if (argv[i][0] != '-') {
            candidates.emplace_back(argv[i]);
        }
    }
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    std::vector<std::filesystem::path> dirs = { cwd, cwd.parent_path() };
    // An AppImage runs from a mount of itself, so "next to the executable" means next to the .AppImage file.
    if (const char* appimage = getenv("APPIMAGE"); appimage != nullptr && appimage[0] != '\0') {
        dirs.push_back(std::filesystem::path{ appimage }.parent_path());
    }
    for (const auto& dir : dirs) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            auto ext = entry.path().extension().string();
            if (ext == ".z64" || ext == ".n64" || ext == ".v64") {
                candidates.emplace_back(entry.path());
            }
        }
    }

    for (const auto& path : candidates) {
        if (recomp::select_rom(path, game_id) == recomp::RomValidationError::Good) {
            printf("Imported ROM from %s\n", path.string().c_str());
            return;
        }
    }
}

std::string get_game_thread_name(const OSThread* t) {
    return "[Game] " + std::to_string(t->id);
}

// Each also tells rush2::derive, which makes the replacements of textures the game changes at run time (repainted car
// panels, stamped stripes) from the enabled packs: without it, toggling a pack in the Mods menu left those as they were.
void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    recompui::renderer::enable_texture_pack(context, mod);
    rush2::derive::packs_changed();
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    recompui::renderer::disable_texture_pack(mod);
    rush2::derive::packs_changed();
}

void reorder_texture_pack(recomp::mods::ModContext&) {
    recompui::renderer::trigger_texture_pack_update();
    rush2::derive::packs_changed(); // The first pack listing a hash wins.
}

// --input-script "<seconds>:<button>[:<hold seconds>],..." presses N64 buttons at fixed times after
// launch, on top of real input (used for automated testing). Buttons: A B Z START L R CU CD CL CR DU DD DL DR, plus SL
// and SR for the stick fully left and right (to steer), for controller 1, or with a P2-P4 prefix (P2START) for
// controllers 2-4.
struct ScriptedPress {
    double time;
    double duration;
    uint16_t button;
    int controller;
    float stick_x = 0.0f;
};
static std::vector<ScriptedPress> input_script;
static std::chrono::steady_clock::time_point launch_time = std::chrono::steady_clock::now();

static void parse_input_script(const std::string& script) {
    static const std::pair<const char*, uint16_t> buttons[] = {
        {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"DU", 0x0800}, {"DD", 0x0400},
        {"DL", 0x0200}, {"DR", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CU", 0x0008}, {"CD", 0x0004},
        {"CL", 0x0002}, {"CR", 0x0001},
    };
    size_t pos = 0;
    while (pos < script.size()) {
        size_t end = script.find(',', pos);
        std::string entry = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? script.size() : end + 1;
        size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        ScriptedPress press{ std::stod(entry.substr(0, colon)), 0.15, 0, 0 };
        std::string name = entry.substr(colon + 1);
        size_t colon2 = name.find(':');
        if (colon2 != std::string::npos) {
            press.duration = std::stod(name.substr(colon2 + 1));
            name = name.substr(0, colon2);
        }
        if (name.size() > 2 && name[0] == 'P' && name[1] >= '2' && name[1] <= '4') {
            press.controller = name[1] - '1';
            script_uses_controller[press.controller] = true;
            rush2::input::set_port_scripted(press.controller);
            name = name.substr(2);
        }
        for (const auto& [button_name, mask] : buttons) {
            if (name == button_name) {
                press.button = mask;
            }
        }
        if (name == "SL" || name == "SR") {
            press.stick_x = name == "SL" ? -1.0f : 1.0f;
        }
        input_script.push_back(press);
    }
}

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    // A simulation run takes only the script's presses, on a player 1 that is always connected (src/sim_run.cpp), and
    // the test virtual pad's (RUSH2_TEST_VPAD).
    bool script_only = rush2::sim::active() && !rush2::input::is_test_vpad_port(controller_num);
    bool ret = script_only ? controller_num == 0 : rush2::input::get_n64_input(controller_num, buttons, x, y);
    if (script_only) {
        *buttons = 0;
        *x = *y = 0.0f;
    }
    // A port the script presses on is a connected controller from the start, so the game doesn't pause for it.
    if (controller_num >= 0 && controller_num < 4 && script_uses_controller[controller_num]) {
        ret = true;
    }
    if (!input_script.empty() && !ultramodern::is_turbo()) { // in turbo a press would last seconds of race time
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch_time).count();
        for (const auto& press : input_script) {
            if (press.controller == controller_num && now >= press.time && now < press.time + press.duration) {
                *buttons |= press.button;
                if (press.stick_x != 0.0f) {
                    *x = press.stick_x;
                }
                rush2::input::press_pedals(controller_num, press.button);
                ret = true;
            }
        }
    }
    return ret;
}

// The launcher only appears when no valid ROM is stored, offering to locate one. Once a ROM is valid (found at startup
// or just picked), the game starts right away.
// The game's controller reads: as get_n64_input, but an online lobby turns ports off, masks buttons or presses them
// for the player (src/online/online_lobby.cpp). The online race reads this machine's controller with get_n64_input.
static bool get_game_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    bool ret = get_n64_input(controller_num, buttons, x, y);
    if (rush2::online::game::controller_override(controller_num, buttons, x, y)) {
        return controller_num == 0 ? true : ret;
    }
    return ret;
}

// The launcher's progress screen for the launch's build pass (rush2::prewarm): a bar with a line under it saying what
// is being built, in place of the launcher's buttons, under the logo. While the pass runs the launcher shows nothing
// (its buttons are for locating a ROM), and the screen comes up only once something is being built (a cache lookup
// missed, an arcade mod is rebuilt: Progress::building), so a launch with everything built goes straight to the game
// on any computer.
namespace {
    recompui::Element* launcher_menu = nullptr;
    recompui::Element* build_screen = nullptr;
    recompui::Label* build_label = nullptr;
    recompui::Element* build_fill = nullptr;
    std::vector<recompui::Element*> launcher_buttons;

    void add_build_screen(recompui::Element* parent) {
        using namespace recompui;
        ContextId context = get_current_context();
        build_screen = context.create_element<Element>(parent, 0, "div", false);
        // In the layout from the start, only invisible, and the buttons only made invisible when it shows: the menu
        // sits from the bottom, so a change of its height would move the logo.
        build_screen->set_visibility(Visibility::Hidden);
        build_screen->set_flex_direction(FlexDirection::Column);
        build_screen->set_align_items(AlignItems::Center);
        build_screen->set_width(640.0f);
        build_screen->set_margin_left_auto();
        build_screen->set_margin_right_auto();
        Element* track = context.create_element<Element>(build_screen, 0, "div", false);
        track->set_width(100.0f, Unit::Percent);
        track->set_height(14.0f);
        track->set_border_radius(7.0f);
        track->set_background_color(theme::color::TextA20);
        track->set_position(Position::Relative);
        // Pinned to the track's left edge. No overflow clipping on the track: with its rounded corners only the end of the
        // fill showed.
        build_fill = context.create_element<Element>(track, 0, "div", false);
        build_fill->set_position(Position::Absolute);
        build_fill->set_left(0.0f);
        build_fill->set_top(0.0f);
        build_fill->set_width(0.0f, Unit::Percent);
        build_fill->set_height(100.0f, Unit::Percent);
        build_fill->set_border_radius(7.0f);
        build_fill->set_background_color(theme::color::Primary);
        build_label = context.create_element<Label>(build_screen, "", theme::Typography::LabelSM);
        build_label->set_text_transform(TextTransform::Uppercase);
        build_label->set_color(theme::color::Text);
        build_label->set_text_align(TextAlign::Center);
        build_label->set_margin_top(12.0f);
    }

    float build_filled = 0.0f;   // the bar's fill as drawn, 0-1

    // Updates the progress screen; true once the game can start: the pass is over and, if the screen came up, its bar
    // has run to the end.
    bool show_build_progress(bool ready) {
        using namespace recompui;
        static bool hidden = false, shown = false;
        if (build_screen == nullptr) return ready;
        if (!hidden) {
            hidden = true;   // a ROM located just now: its buttons go
            launcher_menu->set_visibility(Visibility::Hidden);
        }
        rush2::prewarm::Progress p = rush2::prewarm::progress();
        if (!shown && !p.building) return ready;
        if (!shown) {
            shown = true;
            for (Element* b : launcher_buttons) b->set_visibility(Visibility::Hidden);
            launcher_menu->set_visibility(Visibility::Visible);
            build_screen->set_visibility(Visibility::Visible);
        }
        static std::string text;
        std::string now = !p.job.empty() ? "Building " + p.job : p.finished ? "Starting" : "Getting ready";
        if (now != text) {
            text = now;
            build_label->set_text(text);
        }
        // Eased toward the pass's progress every frame, never back, so the bar moves smoothly left to right; once the
        // pass is over it runs to the end quickly.
        float target = ready || p.finished ? 1.0f : p.fraction;
        if (target > build_filled) {
            float step = ready ? std::max(0.03f, (target - build_filled) * 0.18f) : std::max(0.004f, (target - build_filled) * 0.12f);
            build_filled = std::min(target, build_filled + step);
            build_fill->set_width(100.0f * build_filled, Unit::Percent);
        }
        return ready && build_filled >= 1.0f;
    }
}

static void launcher_init(recompui::LauncherMenu* menu) {
    rush2::online::game::init_overlay();
    const recomp::GameEntry& game = supported_games[0];
    auto game_options_menu = menu->init_game_options_menu(game.game_id, game.mod_game_id, game.display_name,
                                                          game.thumbnail_bytes, recompui::GameOptionsMenuLayout::Center);
    // The Rush 2 Recompiled artwork (icons/logo.png, copied to assets/ by tools/build_icons.py) above the buttons.
    std::ifstream logo_file{ recompui::file::get_program_path() / "assets" / "logo.png", std::ios::binary };
    std::vector<char> logo_bytes{ std::istreambuf_iterator<char>(logo_file), std::istreambuf_iterator<char>() };
    if (!logo_bytes.empty()) {
        // "?/" keeps RmlUi from resolving the name against the document's folder, which on Linux (a relative
        // assets/ path) turned it into assets/rush2_launcher_logo and missed the image; the frontend's thumbnails do
        // the same.
        recompui::queue_image_from_bytes_file("?/rush2/launcher_logo", logo_bytes);
        auto* logo = recompui::get_current_context().create_element<recompui::Image>(game_options_menu,
                                                                                    "?/rush2/launcher_logo");
        logo->set_width(640.0f);
        logo->set_height(640.0f);
        logo->set_margin_bottom(16.0f);
        // The logo replaces the default text title, and the menu sits low (12% up) to make room for it.
        menu->remove_default_title();
        game_options_menu->set_bottom(12.0f, recompui::Unit::Percent);
    }
    add_build_screen(game_options_menu);
    launcher_menu = game_options_menu;
    // With a valid ROM the game starts once the build pass is over: the menu isn't shown meanwhile.
    std::u8string game_id = game.game_id;
    if (recomp::is_rom_valid(game_id)) game_options_menu->set_visibility(recompui::Visibility::Hidden);
    // Never Start Game: with a ROM the launcher starts the game itself (launcher_update).
    launcher_buttons.push_back(game_options_menu->add_start_game_or_load_rom_option("Locate ROM", "Locate ROM"));
    launcher_buttons.push_back(game_options_menu->add_exit_option());
}

static void launcher_update(recompui::LauncherMenu*) {
    static bool started = false;
    const recomp::GameEntry& game = supported_games[0];
    std::u8string game_id = game.game_id;
    if (started) {
        return;
    }
    // The ROM Locate ROM just stored: librecomp marks a ROM valid only when it checks the stored ones, so they're checked
    // again once the file is there, and the launcher goes straight on to the build pass and the game (it never offers
    // Start Game).
    if (!recomp::is_rom_valid(game_id)) {
        static std::filesystem::file_time_type checked{};
        std::error_code ec;
        auto written = std::filesystem::last_write_time(recomp::get_config_path() / game.stored_filename(), ec);
        if (ec || written == checked) {
            return;
        }
        checked = written;
        recomp::check_all_stored_roms();
        if (!recomp::is_rom_valid(game_id)) {
            return;
        }
    }
    // A fresh install's setup screen first (src/setup.cpp): its other games step can add ROMs the build pass then
    // builds. The launcher shows again, and comes back here, once it closes.
    if (rush2::setup::pending()) {
        static bool queued = false;
        if (!queued) {
            queued = true;
            recompui::queue_ui_task([]() { rush2::setup::open(true); });
        }
        return;
    }
    // Every car and track is built before the game starts (src/prewarm.cpp).
    rush2::prewarm::start(game_id);
    if (!show_build_progress(rush2::prewarm::ready())) {
        return;
    }
    started = true;
    recompui::update_game_mod_id(game.mod_game_id);
    recomp::start_game(game_id, {});
    recompui::hide_all_contexts();
}

int main(int argc, char** argv) {
    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

#ifdef _WIN32
    // Set up high resolution timing period.
    timeBeginPeriod(1);
    rush2::hitch::start();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--show-console") == 0) {
            if (GetConsoleWindow() == nullptr) {
                AllocConsole();
                freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stderr);
                freopen("CONOUT$", "w", stdout);
            }
            break;
        }
    }

    SetConsoleOutputCP(CP_UTF8);

    // UI assets are loaded relative to the working directory, so run from the executable's folder.
    wchar_t module_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) != 0) {
        std::error_code ec;
        std::filesystem::current_path(std::filesystem::path{ module_path }.parent_path(), ec);
    }

    // Force wasapi on Windows, as there seems to be some issue with sample queueing with directsound currently.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#elif defined(__linux__)
    // As on Windows, run from the executable's folder (inside an AppImage, its read-only usr/bin).
    {
        std::error_code ec;
        std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            std::filesystem::current_path(exe.parent_path(), ec);
        }
    }
#endif

#ifdef __linux__
    rush2::linux_window::prepare_gtk(); // Before NFD_Init starts GTK.
#endif
    NFD_Init();
#ifdef __linux__
    recompui::set_folder_opener(rush2::linux_window::open_folder);
#endif

    recompui::programconfig::set_program_name(rush2::program_name);
    recompui::programconfig::set_program_id(rush2::program_id);

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    std::u8string controller_db_path = (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");

    if (rush2::sim::active() && rush2::sim::data_dir().empty()) {
        // Never race a simulation on the player's own settings and save.
        fprintf(stderr, "RUSH2_SIM needs RUSH2_DATA_DIR (the run's own data folder)\n");
        return EXIT_FAILURE;
    }
    if (!rush2::sim::data_dir().empty()) {
        // A simulation run keeps its own settings and save (src/sim_run.cpp), and never moves the player's data.
        std::error_code ec;
        std::filesystem::create_directories(rush2::sim::data_dir(), ec);
        recomp::register_config_path(rush2::sim::data_dir());
    }
    else {
        rush2::data_location::apply_pending_move();
        recomp::register_config_path(recompui::file::get_app_folder_path());
    }
    rush2::data_files::migrate();
    rush2::install_font_pack();

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }
    auto_import_rom(argc, argv);

    recompui::register_ui_exports();

    rush2::register_overlays();

    recompinput::players::set_single_player_mode(true);

    rush2::init_config();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
            parse_input_script(argv[++i]);
            rush2::input::set_rumble_muted(true);
        }
        else if (strcmp(argv[i], "--debug-rumble") == 0) {
            rush2::input::set_rumble_debug(true);
        }
    }
    if (rush2::sim::active()) {
        rush2::input::set_rumble_muted(true);
    }
    recompui::register_launcher_init_callback(launcher_init);
    recompui::register_launcher_update_callback(launcher_update);
    rush2::track_mod::register_mod_notes();

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            rush2::census::set_rdram(rdram);
            // RT64 keeps every specialized shader it compiles here and builds them all at launch (docs/hitch_profiler.md).
            RT64::RasterShaderCache::setDiskCacheDirectory(recomp::get_config_path() / "shader_cache");
            auto presentation_mode = ultramodern::renderer::PresentationMode::PresentEarly;
            return recompui::renderer::create_render_context(rdram, window_handle, presentation_mode, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    rush2::online::game::set_input_source(get_n64_input);

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = rush2::input::poll,
        .get_input = get_game_input,
        .set_rumble = rush2::input::set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = [] {
            rush2::input::update_rumble();
            rush2::light_bar::update();
        },
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    // Register the texture pack content type with rt64.json as its content file.
    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_pack,
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);
    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    NFD_Quit();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return EXIT_SUCCESS;
}
