#ifndef __WINGS_H__
#define __WINGS_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Rush 2049 wings (src/wings*.cpp).
namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::wings {
    // The Rush 2049 settings, shown in the Games tab (src/games_tab.cpp) and the wing styles in the Players tab.
    // init_config adds them; call it before recompui::config::finalize().
    void init_config();
    // Adds the Rush 2049 section (ROM picker and options) to the Games tab; refresh keeps its ROM status current.
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    void save_config();
    // A player's (0 or 1) wing style option, 0-2; setting it saves the settings.
    int get_style_option(int player);
    void set_style_option(int player, int style);
    // Loads the saved settings and the stored Rush 2049 ROM. Call after recompui::config::finalize().
    void load_config();

    // The big-endian Rush 2049 (USA) ROM, or null if none has been provided.
    std::shared_ptr<const std::vector<uint8_t>> get_rom();
    bool rom_available();
    // True when the Wings option is on and the Rush 2049 ROM is available.
    bool enabled();

    // Rebuilds the wing model and sound from the current ROM (src/wings_render.cpp).
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
    void set_ghost_input(int car, bool active, const Input& in);

    // Mixes the playing wing sounds into the game's audio output (src/wings_sound.cpp). samples is interleaved
    // stereo, sample_count values in total, at sample_rate frames per second; each 16-bit PCM unit is scaled by
    // pcm_scale.
    void mix_sound(float* samples, size_t sample_count, uint32_t sample_rate, float pcm_scale);
}

#endif
