#ifndef __ASSET_CATALOG_H__
#define __ASSET_CATALOG_H__

// The game's models and textures read straight from the ROMs, with names (docs/asset_catalog.md).
//
// Every model the game draws comes from a Rush 2 model container (docs/rush2049_research/geometry.md section 2): Rush 2's
// own assets, and the SF Rush and Rush 2049 tracks and cars the recomp converts to Rush 2's format. read_container runs
// a container's display lists the way the RSP and RDP would: it loads TMEM like RT64 does, so each texture comes out
// with the exact RT64 replacement hash and decoded pixels the game draws it with, and collects the triangles for
// write_glb. src/asset_catalog_walk.cpp has that part, which needs nothing of the game; src/asset_catalog.cpp goes
// through every source and writes the dumps.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "texture_origin.h"
#include "texture_upscale.h"

namespace rush2::catalog {
    using rush2::upscale::Edge;
    using rush2::upscale::Image;

    // A texture as a container's lists (or its texture table) draw it.
    struct Texture {
        // Unique in the container: the texture table's name for these texels, else the first model drawing it ("NAME"
        // when it draws one new texture, else "NAME.1", "NAME.2"...). Another palette or tile of the same texels
        // gets "~2", "~3"...; a mipmap level of the one before it ".mip1"...
        std::string name;
        uint32_t texels = 0;        // File offset of the texels (the load that filled the render tile's TMEM).
        uint32_t width = 0, height = 0;
        uint8_t fmt = 0, siz = 0;
        uint32_t tlut = 0;          // G_TT_* of the draw (0: none).
        Edge edge_s = Edge::Wrap, edge_t = Edge::Wrap;
        // RT64's replacement hash of the draw (TMEMHasher version 5), 0 when the file alone can't give it: a texture
        // table texture the game's texture-load builder (func_8007825C) loads, or one RT64 decodes from raw TMEM.
        uint64_t hash = 0;
        uint64_t key = 0;           // rush2::upscale::content_key of image.
        uint64_t index_key = 0;     // Paletted: rush2::upscale::index_key (the same for every palette, car paint).
        bool painted = false;       // Its palette isn't in the file (the game writes it: car paint). image is gray.
        std::vector<uint8_t> indices; // Paletted: the palette index of each texel, row by row (rush2::texsource).
        Image image;
        std::vector<std::string> models; // The models drawing it, in container order.
    };

    struct Vertex {
        float position[3];
        float uv[2];                // In units of the texture's size (0..1 across it once).
        uint8_t color[4];
    };

    // Triangles (every 3 vertices) drawn with one texture and wrap mode.
    struct Mesh {
        int texture = -1;           // Index into Container::textures, or -1.
        bool lit = false;           // Vertex colors are normals (2049 LOD flag 0x10).
        std::vector<Vertex> vertices;
    };

    struct Lod {
        float distance = 0;
        uint16_t flags = 0;
        std::vector<Mesh> meshes;
    };

    struct Model {
        std::string name;
        float radius = 0;
        uint16_t kind = 0;          // Breakable class (name record +0x14).
        std::vector<Lod> lods;
    };

    struct Container {
        std::vector<Model> models;
        std::vector<Texture> textures;
    };

    // A record of a Rush 2 placement file (docs/rush2049_research/placement.md section 1): a model (or object class)
    // placed in the world. matrix is rotation and scale (rows), position the world position.
    struct Placement {
        std::string name;
        float matrix[9];
        float position[3];
        uint32_t flags = 0;
    };
    std::vector<Placement> read_placement(const std::vector<uint8_t>& file);

    // Reads a Rush 2 model container (file offsets, as stored). geometry: also collect the triangles (for write_glb).
    // paint_records: show a painted texture with the file's paint palette record when it has some (SF Rush's cars:
    // the RED one) instead of the gray ramp; for the model dump's SF Rush files only, as the texture dump's recoloring
    // needs the gray and the 2049 cars' palette records cover only part of a palette.
    // copies: texel offsets the code points the lists' loads at in place of others (texels -> copy: a 2049 car's
    // scuffed panels, Entry::copies); each copy is cataloged as its texture with _D1.
    // Every texture-load list of the file is read too, called or not (LOADLIST.n when no record names it).
    // False with error set when it isn't one.
    bool read_container(const std::vector<uint8_t>& file, Container& out, std::string& error, bool geometry = true,
                        bool paint_records = false, const std::map<uint32_t, uint32_t>* copies = nullptr);

    // A container as glTF 2.0 binary: one node per model (its first LOD; the others as nodes "<model> LOD n" under
    // it, hidden by scale 0 so viewers show one), the textures embedded as PNG under their names. With placements (a
    // track), scene 0 is the track as placed (each record drawing its model's first LOD) and scene 1 every model at
    // the origin; without, scene 0 is the models.
    std::vector<uint8_t> write_glb(const Container& container, const std::string& title,
                                   const std::vector<Placement>& placements = {});

    // Model m of a container alone, with just the textures it draws (for one .glb per model).
    Container single_model(const Container& container, size_t m);

    // A name made safe for a file name (letters, digits, '.', '_', '-', '~').
    std::string file_name(const std::string& name);

    // The game side (src/asset_catalog.cpp).

    // Whether a container path (Entry::path) holds interface art: menus, the HUD and pause screens, track maps, stunt
    // icons. Their textures are left out of the dump and the upscaling unless Upscale Interface is on.
    bool ui_path(const std::string& path);

    // One container, with its place in the dumps: "<game folder>/<path>" (rush2/cars/PICKUP...).
    struct Entry {
        rush2::origin::Game game;   // Where most of it comes from (the dump folder of its models).
        std::string path;
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::shared_ptr<const std::vector<uint8_t>> placement; // A track's placement file, or null.
        // Only its placed scene goes in the model dump: a converted Rush 2049 track, whose models are dumped from the
        // 2049 files under their own names.
        bool placed_only = false;
        // Parts from another game than `game` (the SF Rush and Rush 2049 logos added to Rush 2's menu containers):
        // a texture's dump folder is the game its texels come from.
        struct Span {
            uint32_t offset, size;
            rush2::origin::Game game;
        };
        std::vector<Span> spans;
        rush2::origin::Game game_at(uint32_t offset) const;
        // Texels the game swaps in for others (read_container's copies): a 2049 car's scuffed panels.
        std::map<uint32_t, uint32_t> copies;
    };
    // Every container the game can draw from: Rush 2's assets (with the recomp's replacements: the 2049 cars and the
    // extended menu containers) and every SF Rush and Rush 2049 track converted afresh. source_files: also every model
    // file of the SF Rush and Rush 2049 ROMs under its own names (menus, shared objects, effects, weapons, cars, each
    // track's objects; the model dump), in place of the 2049 car slots. Runs on the calling thread for a few seconds;
    // status gets what it's on.
    std::vector<Entry> containers(const std::function<void(const std::string&)>& status, bool source_files = false);

    // Dump Models (Settings > Graphics): writes every model of every container (source_files included) as
    // <app folder>/asset_dump/models/<game>/<path>/<model>.glb, plus <path>/_all.glb with all of a container's models and,
    // for a track, <path>/_placed.glb with the track as placed. In the background; returns false if one is running.
    bool start_model_dump();
    bool model_dump_running();
    std::string model_dump_status();  // What it's on, then how many it wrote.
    std::filesystem::path model_dump_dir();
}

#endif
