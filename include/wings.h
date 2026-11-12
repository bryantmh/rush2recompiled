#ifndef __WINGS_H__
#define __WINGS_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "rush2049_rom.h"

// Rush 2049 wings (src/rush2049/wings*.cpp).
namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::wings {
    // The Rush 2049 settings, shown in the Games tab (src/games_tab.cpp). init_config adds them; call it before recompui::config::finalize().
    void init_config();
    // Adds the Rush 2049 section (ROM picker and options) to the Games tab; refresh keeps its ROM status current.
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    // Adds Rush 2049 Menus and its Fonts, Sounds and Background switches, at the end of the section.
    void add_menu_options(rush2::ui::OptionsPage* page);
    void save_config();
    // A player's (0-3) wing style, 0-2, chosen on the car select and kept in the saves' "wings" section
    // (src/rush2049/wings_menu.cpp); setting it saves it. load_styles reads them at startup.
    int saved_style(int player);
    void save_style(int player, int style);
    void load_styles();
    // Loads the saved settings, the wing styles and the stored Rush 2049 ROM. Call after recompui::config::finalize().
    void load_config();

    // The Rush 2049 data: the N64 (USA) ROM or the Dreamcast (USA) disc the player provided, or null if neither.
    std::shared_ptr<const rush2::rom2049::Source> get_rom();
    bool rom_available();
    // Whether both the N64 ROM and the Dreamcast disc are in the app folder (the Games tab's Enabled Source and Primary Source).
    bool both_sources();
    // The N64 ROM or the Dreamcast disc (disc), opened from the app folder unless it's the one in use; null if it
    // isn't there. Reads the ROM from disk: not for the game thread. For the Sound tab's Rush 2049 Soundtrack.
    std::shared_ptr<const rush2::rom2049::Source> open_source(bool disc);
    // Whether the app folder holds the N64 ROM / the Dreamcast disc (as the Games tab last found; no disk access).
    bool stored(bool disc);
    // Whether the game may use it: stored, and the Games tab's Enabled Source allows it (it only chooses when both
    // are stored).
    bool source_enabled(bool disc);
    // The N64 ROM or the Dreamcast disc, for the tracks, which offer both at once (rush2::track2049::courses_from_disc):
    // the one in use, else the other opened once and kept; null if it isn't stored or enabled (source_enabled).
    // Opening it reads it from disk: the launch's build pass (rush2::track2049::prepare) opens it before the game asks.
    std::shared_ptr<const rush2::rom2049::Source> track_source(bool disc);
    // The height of a Rush 2 car type's roof (0-21; world units, from the car models), 3.5 for any other type.
    float body_height(int type);
    // True when the Wings option is on and the Rush 2049 ROM is available.
    bool enabled();

    // Rebuilds the wing model and sound from the current ROM (src/rush2049/wings_render.cpp).
    void on_rom_changed();

    // Ghost races (src/ghost.cpp), physics thread. What the player driving race car `car` (0-7) holds for its wings
    // (the button, the stick and the player's wing style); false for a car without a player. A ghost car has no
    // player: set_ghost_input gives it the recorded values for its next physics step (active = false stops that).
    struct Input {
        bool held = false;
        float stick_x = 0.0f;
        float stick_y = 0.0f;
        int style = 0;
    };
    bool car_input(uint8_t* rdram, int car, Input& out);
    // Physics thread: whether race car `car` (0-7) had its wings out in its last physics step (Rush 2049's car +0x3F8).
    bool gliding(int car);
    void set_ghost_input(int car, bool active, const Input& in);

    // Mixes the playing wing sounds into the game's audio output (src/rush2049/wings_sound.cpp). samples is interleaved
    // stereo, sample_count values in total, at sample_rate frames per second; each 16-bit PCM unit is scaled by
    // pcm_scale.
    void mix_sound(float* samples, size_t sample_count, uint32_t sample_rate, float pcm_scale);
}

#endif
