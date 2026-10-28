#ifndef __CAR1_STRIPES_H__
#define __CAR1_STRIPES_H__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "asset_catalog.h"

// The "SF Rush" STRIPE choice: Rush 1's own decals (src/rush1/car1_decals.cpp) as a ninth STRIPE value on the four cars that
// have one (src/rush1/car1_stripes.cpp, docs/rush1_research.md section 11). Needs the user's Rush 1 ROM.
namespace rush2::car1stripes {
    // The SF Rush Car Stripes option (Games tab, with the SF Rush Tracks option).
    void set_option(bool enabled);

    // The STRIPE value of the SF Rush stripe (Rush 2's are 0-7).
    constexpr int value = 8;
    // Whether car type `type` offers the SF Rush stripe: the option is on, the car has a Rush 1 decal and the Rush 1
    // ROM is there (the decal itself is built on first use).
    bool offered(int type);
    // Builds every car's decal (or reads it from the disk cache, stripe_cache) before the game starts: the launch's
    // build pass (src/prewarm.cpp), with the Rush 1 ROM in.
    void prepare();
    // Whether car type `type` can be painted with it now, building (or loading) its decal: game thread.
    bool available(uint8_t* rdram, int type);

    // The SF Rush decal of a Rush 2 car panel, as the game writes it (docs/texture_upscaling.md "SF Rush decals"):
    // Dump Textures writes the decal alone to upscale, as the stripe tiles are, and the derived replacements lay its
    // upscale over the plain panel's.
    struct DecalTexture {
        size_t texture = 0;                 // The plain panel's index in the container's textures.
        std::vector<uint8_t> mask;          // Per texel: 1 where the decal is.
        std::vector<uint8_t> indices;       // The panel's palette indices with the decal's (a STRIPE COLOR decal's
                                            // texels, which the game remaps per color, keep the panel's).
        bool stripe_color = false;          // The decal takes the STRIPE COLOR (else its own colors).
        rush2::upscale::Image image;        // The decal alone, transparent off it: in its own colors (the paint ramps
                                            // in the preview colors), or flat cyan for one in the STRIPE COLOR. The
                                            // same for the clean and the damaged panel.
        uint64_t key = 0;                   // Stands for the image as a catalog content key (names, packs).
    };

    // The decal panels of a Rush 2 car container (read without geometry; preview_palette: its
    // rush2::catalog::paint_preview_palette). Empty for a car without an SF Rush decal or without the Rush 1 ROM. The
    // decals come from the same disk cache as the game's (stripe_cache), built once per ROM and build.
    std::vector<DecalTexture> decal_textures(const rush2::catalog::Entry& car, const rush2::catalog::Container& c,
                                             const std::vector<rush2::catalog::Entry>& containers,
                                             const std::vector<uint8_t>& preview_palette);
}

#endif
