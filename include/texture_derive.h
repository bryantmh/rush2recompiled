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
}

#endif
