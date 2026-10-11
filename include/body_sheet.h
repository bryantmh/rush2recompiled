// Half-body sheets (src/body_sheet.cpp, docs/texture_upscaling.md "Half-body sheets"): a Rush 2049 car's body is
// drawn with 15-25 small tiles (the N64's texture memory holds one 32x16 CI8 tile at a time), each used on both sides
// of the car. Dump Textures lays one side's tiles out as one image, joined where they meet on the body, so an upscale of
// it runs across the seams; Install cuts an upscale of the sheet back into the tiles, pixel for pixel.
#ifndef RUSH2_BODY_SHEET_H
#define RUSH2_BODY_SHEET_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "asset_catalog.h"

namespace rush2::sheet {
    using rush2::upscale::Image;

    // Part of a tile laid on the sheet: sheet = M[orient] * tile + (x, y), in texels (a tile texel's center lands on a
    // sheet texel's center). Turns are whole quarter turns and flips, so nothing is resampled.
    struct Piece {
        std::string tile;                   // The tile's dumped path in its game folder ("cars/PANTHER/C13_64_1").
        uint32_t tile_width = 0, tile_height = 0;
        int orient = 0;                     // 0-7: rotations by 0, 90, 180, 270 degrees, then the same flipped.
        int x = 0, y = 0;
        std::vector<uint8_t> shown;         // Tile texels drawn on the sheet (tile_width * tile_height, 0/1).
        std::vector<uint8_t> core;          // Of those, the ones the body's triangles cover (the rest is a 1-texel rim).
        std::vector<uint8_t> owned;         // Tile texels this piece gives back to the tile at Install.
    };

    struct Sheet {
        std::string path;                   // Its dumped path in the game folder ("cars/PANTHER/PANTHER_BODY").
        uint32_t width = 0, height = 0;
        std::vector<Piece> pieces;
    };

    // Lays out the sheet of a car container read with geometry: the triangles of its body (Rush 2049: the model whose
    // name ends in FRAME1, first LOD; Rush 2: the D0_ FL1, FR1, RL1, RR1 and TOP1 parts) drawn with a texture that has
    // a dumped path (paths: per texture index, "" for none), plus any other near model's pieces of those tiles showing
    // texels the body doesn't. drawn_masks, when given, gets every tile's texels the car draws (any model, any
    // distance), by dumped path. False when there is no body.
    bool build(const rush2::catalog::Container& container, const std::vector<std::string>& paths,
               const std::string& sheet_path, Sheet& out,
               std::map<std::string, std::vector<uint8_t>>* drawn_masks = nullptr);

    // Whether the sheet gives back every texel of a tile the car draws (drawn: from build's drawn_masks).
    bool covers(const Sheet& sheet, const std::string& tile, const std::vector<uint8_t>& drawn);

    // The sheet's image from the tiles' dumped images (image_of: a tile path's image, or null). Space between the
    // pieces is filled from their edges, so an upscaler sees no hard borders there.
    Image render(const Sheet& sheet, const std::function<const Image*(const std::string&)>& image_of);

    // A tile's upscale cut from an upscale of the sheet (a whole multiple of the sheet's size). Texels no piece gives
    // back come from fallback (the tile's own upscale), else from tile_image scaled up. False if the sheet has no piece
    // of the tile.
    bool cut(const Sheet& sheet, const Image& sheet_up, const std::string& tile, const Image& tile_image,
             const Image& fallback, Image& out);

    // The tiles a sheet has pieces of.
    std::vector<std::string> tiles(const Sheet& sheet);

    // body_sheets.txt in a dump's game folder: every sheet's layout, which Install reads.
    std::string write_layout(const std::vector<Sheet>& sheets);
    std::vector<Sheet> read_layout(const std::string& text);
}

#endif
