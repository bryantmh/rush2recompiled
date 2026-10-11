#ifndef __TEXTURE_DERIVE_H__
#define __TEXTURE_DERIVE_H__

// Replacements for the textures the game makes at run time from the ROMs' (src/texture_derive.cpp,
// docs/texture_upscaling.md "Derived replacements"). A texture pack replaces the catalog's textures (rush2::catalog,
// the ROMs' own) by their RT64 hashes; what the game draws besides them is always one of those with something
// changed: another palette (car paint, cycling signs), texels stamped over (stripes), a copy elsewhere in RDRAM
// (speed signs), part of one (strips) or one with neighbouring memory past its end (a tile taller than the load).
// Each texture drawn that no pack replaces is traced to its catalog texture (by content key, palette indices, or the
// rows of texels it shares) and replaced live with that texture's upscale from the enabled packs, recolored to what
// is drawn.

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "texture_upscale.h"

namespace rush2::derive {
    // Starts building the catalog's index in the background.
    void start();
    // A texture drawn for the first time under hash (the upscaler's observer, any draw). indices are its palette
    // indices when it has a palette (else empty); siz is the tile's G_IM_SIZ; world is false for a 2D draw.
    void drawn(uint64_t hash, uint64_t key, uint64_t index_key, const rush2::upscale::Image& image,
               std::vector<uint8_t> indices, uint8_t siz, bool world);
    // Upscale Interface (Settings > Graphics): with it off, interface textures (drawn in 2D, or only in the catalog's
    // interface containers, rush2::catalog::ui_path) get no derived replacement.
    void set_ui(bool enabled);
    // Keep Car Colors (Settings > Graphics, on by default): car paint replacements keep only colors their texels have
    // nearby (rush2::upscale::clamp_chroma), so an upscaler's color spill onto fixed parts (lights, trim) goes.
    void set_keep_car_colors(bool enabled);
    // Whether the catalog's index is built, so interface_texture can answer.
    bool index_ready();
    // Whether a texture with this content key or these palette indices is only in interface containers.
    bool interface_texture(uint64_t key, uint64_t index_key);
    // Whether hash has a derived replacement (the upscaling modes leave those alone).
    bool owns(uint64_t hash);
    // What became of a texture drawn (for the texture census): not looked at yet (or no pack enabled), replaced by a
    // pack as drawn, derived from its catalog texture's upscale, traced to a catalog texture no pack has an upscale
    // of, not traced to the catalog, interface art left alone (Upscale Interface off), or too small or flat to
    // upscale (never dumped: rush2::upscale::worth_upscaling).
    enum class Outcome : uint8_t { Unknown, Pack, Derived, NoUpscale, Untraced, Interface, Small };
    Outcome outcome(uint64_t hash);
    // The enabled texture packs changed (Install Upscaled Textures, the mods menu): read them again and derive anew.
    void packs_changed();

    // Whether built-in upscaling (rush2::upscale::builtin_scale) leaves a texture to the derived replacements: a car
    // texture by its content key or palette index key.
    bool builtin_car(uint64_t key, uint64_t index_key);

    // Closes the derived replacements' handles on the texture packs and keeps them shut while the returned lock is
    // held (Install swaps the custom pack's .rtz: an open file can't be replaced on Windows).
    std::unique_lock<std::mutex> hold_packs();

    // A car mod's texture (src/car_mods.cpp). The car draws a small stand-in in Rush 2's car palette (its paint ramps
    // where the mod paints the car); drawn with these palette indices, it is replaced by the mod's full-size image
    // with its painted texels (mask red = MAIN COLOR, green = ACCENT COLOR) recolored to the paint it is drawn with,
    // read from the stand-in's texels of palette entries main_entry and accent_entry, and scuffed when `damaged` (a
    // damaged panel's copy). `load` decodes the image and mask (once per `id`, on the worker thread).
    struct ModTexture {
        uint64_t id = 0;
        std::function<bool(rush2::upscale::Image& image, rush2::upscale::Image& mask)> load;
        uint8_t main_entry = 1, accent_entry = 33;
        bool damaged = false;
        uint32_t seed = 0;
    };
    // The mods' textures by stand-in: a car's by its index_key, a model mod's (true color, unpainted) by its
    // content_key. Replaces the last set and derives again.
    void set_mod_textures(std::unordered_map<uint64_t, ModTexture> textures);
    // Whether a texture drawn with these indices or this content is a mod's stand-in (the upscaling modes leave
    // those alone).
    bool mod_texture(uint64_t index_key, uint64_t key = 0);
}

#endif
