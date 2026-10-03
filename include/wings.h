#ifndef __WINGS_H__
#define __WINGS_H__

#include <cstdint>
#include <memory>
#include <vector>

// Rush 2049 wings (src/wings*.cpp).
namespace rush2::wings {
    // Adds the Rush 2049 tab. Call from init_config before recompui::config::finalize().
    void create_tab();
    // Loads the tab's saved settings and the stored Rush 2049 ROM. Call after recompui::config::finalize().
    void load_config();

    // The big-endian Rush 2049 (USA) ROM, or null if none has been provided.
    std::shared_ptr<const std::vector<uint8_t>> get_rom();
    bool rom_available();
    // True when the Wings option is on and the Rush 2049 ROM is available.
    bool enabled();

    // Rebuilds the wing model and sound from the current ROM (src/wings_render.cpp).
    void on_rom_changed();

    // Mixes the playing wing sounds into the game's audio output (src/wings_sound.cpp). samples is interleaved
    // stereo, sample_count values in total, at sample_rate frames per second; each 16-bit PCM unit is scaled by
    // pcm_scale.
    void mix_sound(float* samples, size_t sample_count, uint32_t sample_rate, float pcm_scale);
}

#endif
