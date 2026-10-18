#ifndef __CAR_MODS_H__
#define __CAR_MODS_H__

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Car model mods (src/car_mods.cpp, docs/car_mods.md): a mod that holds rush2/cars/<car>.glb (<car> a Rush 2 car's part
// prefix: PICKUP, INTEG, VETTE, ...) replaces that car with the mesh, any number of triangles, drawn as one body that
// dents like a Rush 2049 car's (rush2::car2049::set_body_car). Its textures are drawn at their full size in the car's
// paint (rush2::derive::set_mod_textures). Turning the mod off in the Mods menu brings the car back. A mod's
// rush2/new_cars/<ID>.glb with <ID>.json adds a car to the car select instead (mod car types 57-72,
// docs/car_mods.md#new-cars).
namespace rush2::car_mods {
    // At boot, after the car tables and assets are set up (rush2_heap_init).
    void init(uint8_t* rdram);
    // Once per frame: applies mods turned on or off since, while the game is in the menus.
    void check(uint8_t* rdram);

    // Liveries (docs/car_mods.md#liveries): a mod's rush2/liveries/<ID>.json gives an existing car (a Rush 2 or Rush
    // 2049 car) more looks, each a whole mesh drawn as authored, picked with the car select's STYLE row. STYLE 1 is the
    // car as it is.
    // How many liveries car `type` has (0: no STYLE row).
    int livery_count(int type);
    // The STYLE a profile (its record address in the save area) picked for car `type`: 0 the car itself, n livery n.
    int livery_style(uint8_t* rdram, uint32_t profile, int type);
    void set_livery_style(uint8_t* rdram, uint32_t profile, int type, int style);
    // Shows the picked livery on car `id`'s loaded copy (a car select preview id, or a race car 0-7), or the car's own
    // body for STYLE 1: from the end of func_8008582C (rush2_car49_paint), and when STYLE changes.
    void paint_livery(uint8_t* rdram, int id);
    // The livery body (rush2::car2049::add_livery_body id) car `id`'s copy shows, and the copy's base; -1 for none.
    int livery_body(uint8_t* rdram, int id, uint32_t& base);
}

// Files in the enabled mods (car models, HUD placements).
namespace rush2::mod_files {
    struct ModFile {
        std::string mod_id;
        std::filesystem::path path;  // its folder or .rtz
    };
    // The enabled mods, in load order; `signature` gets every mod's id and whether it's on.
    std::vector<ModFile> enabled_mods(std::string& signature);
    // A file in a mod (a path inside it, "rush2/cars/VETTE.glb"); false if it hasn't one.
    bool read_mod_file(const ModFile& mod, const std::string& name, std::vector<uint8_t>& out);
}

#endif
