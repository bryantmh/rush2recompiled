// Coins and keys on the arcade versions of the stock courses (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions).
//
// An arcade version of a Rush 2049 race track or SF Rush track (the game-built arcade mods, include/arcade2049.h) has
// the N64 course's coins or keys whose spot it has too: tools/arcade_collectibles.py compares the ground under and
// around each one and the walls beside it on both tracks and writes the ones that match into arcade_collectibles.inc.
// The arcade tracks are in the N64 games' world frame, so each keeps its N64 record name (its bit), matrix and position.
// They are the course's own: taken on either version, they count for both (src/collectibles.cpp).
//
// The custom track converter (src/track_mod/track_mod_convert.cpp) copies their models in from the N64 games (Rush
// 2049's GOLDCOING_COIN / SILVERCOINS_COI from file 68, SF Rush's KEYL1 from asset 12, with Rush 2's key behaviour 8)
// and places the records, as the stock courses' converters do (src/rush2049/track2049_convert.cpp,
// src/rush1/track1_convert.cpp). Without the ROM a model comes from, the track has none.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "arcade2049.h"
#include "collectibles.h"
#include "rush2049_rom.h"
#include "track1.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "track_mod_runtime.h"
#include "wings.h"

namespace {
    struct Spot {
        const char* mod_id;
        const char* folder;
        int course;         // collectibles.cpp's: SF Rush tracks 0-6, then Rush 2049 race tracks 7-12
        const char* record; // KEY1-8 (SF Rush key n), KEYS0-7 / KEYG0-7 (Rush 2049 silver / gold coin)
        int bit;
        float m[9];
        float pos[3];
    };
    const Spot spots[] = {
#include "arcade_collectibles.inc"
    };

    constexpr uint16_t key_behaviour = 8;   // Rush 2's key (the KEY class's model behaviour)
    const char* const coin_models[] = { "GOLDCOING_COIN", "SILVERCOINS_COI" };
    constexpr const char* key_model = "KEYL1";

    bool is_spot_of(const Spot& s, const std::string& mod_id, const std::string& folder) {
        return mod_id == s.mod_id && folder == s.folder;
    }

    // The model containers, converted once per ROM.
    std::mutex containers_mutex;
    std::shared_ptr<const rush2::rom2049::Source> coins_rom;
    std::vector<uint8_t> coins_container;
    std::shared_ptr<const std::vector<uint8_t>> keys_rom;
    std::vector<uint8_t> keys_container;
}

int rush2::collectibles::arcade_course(const std::string& mod_id, const std::string& folder) {
    for (const Spot& s : spots) {
        if (is_spot_of(s, mod_id, folder)) return s.course;
    }
    return -1;
}

int rush2::collectibles::raced_arcade_course() {
    int r = rush2::track_mod::race_track();
    if (r < 0) return -1;
    std::vector<rush2::track_mod::Entry> list = rush2::track_mod::tracks();
    return r < (int)list.size() ? arcade_course(list[size_t(r)].mod_id, list[size_t(r)].folder) : -1;
}

bool rush2::arcade2049::collectible_extras(const std::string& mod_id, const std::string& folder,
                                           rush2::track_mod::Extras& out) {
    int course = rush2::collectibles::arcade_course(mod_id, folder);
    if (course < 0) return false;
    bool coins = course >= rush2::collectibles::sfrush_courses;
    std::vector<uint8_t> container;
    {
        std::lock_guard lock{ containers_mutex };
        if (coins) {
            auto rom = rush2::wings::get_rom();
            if (rom == nullptr) return false;
            if (rom != coins_rom) {
                std::string error;
                coins_container.clear();
                if (!rush2::track2049::convert_file(*rom, rush2::track2049::coins_file, coins_container, error)) {
                    printf("[arcade] Rush 2049's coin models couldn't be read: %s\n", error.c_str());
                    coins_container.clear();
                }
                coins_rom = rom;
            }
            container = coins_container;
        }
        else {
            auto rom = rush2::track1::get_rom();
            if (rom == nullptr) return false;
            if (rom != keys_rom) {
                std::string error;
                keys_container.clear();
                if (!rush2::track1::convert_container(*rom, rush2::track1::shared_objects_asset, keys_container, error)) {
                    printf("[arcade] SF Rush's key model couldn't be read: %s\n", error.c_str());
                    keys_container.clear();
                }
                keys_rom = rom;
            }
            container = keys_container;
        }
    }
    if (container.empty()) return false;
    if (coins) {
        for (const char* name : coin_models) out.models.push_back({ container, name, key_behaviour });
    }
    else {
        out.models.push_back({ std::move(container), key_model, key_behaviour });
    }
    // Test aid: RUSH2_TEST_ARCADE_SPOT=<record>,x,y,z puts that coin or key (KEYG2, KEY3, ...) at (x, y, z) instead, to
    // take it on a short drive from the grid (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions).
    char moved[16] = {};
    float at[3] = {};
    const char* test = std::getenv("RUSH2_TEST_ARCADE_SPOT");
    bool move = test != nullptr && std::sscanf(test, "%15[^,],%f,%f,%f", moved, &at[0], &at[1], &at[2]) == 4;
    for (const Spot& s : spots) {
        if (!is_spot_of(s, mod_id, folder)) continue;
        rush2::track_mod::Extras::Record r;
        r.name = s.record;
        std::memcpy(r.m.data(), s.m, sizeof(s.m));
        std::memcpy(r.pos.data(), move && r.name == moved ? at : s.pos, sizeof(s.pos));
        out.records.push_back(std::move(r));
    }
    return true;
}
