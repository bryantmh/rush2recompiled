// SF Rush's keys and Rush 2049's coins on the added tracks (docs/unlocks_plan.md).
//
// Both are picked up the way Rush 2's keys are. The converters place them as Rush 2 KEY records (the KEY class,
// behaviour 8; src/track1_convert.cpp, src/track2049_convert.cpp), so Rush 2's key code runs them: func_800BC3F8
// creates a key, func_8005F118 updates it, func_8005F508 is its pickup. Rush 2 numbers its keys in creation order
// (keys 0-11, Dew cans 12-15) and keeps one u16 mask per track in each player record (+0x18 + track * 2), or for
// players without a profile in the session table 0x800C1DBC. The added tracks race in borrowed slots (HAWAII for the
// race tracks, STUNT1 for the stunt arenas), so while one of them is hosted:
// - Creation (func_800BC3F8 at 0x800BC490): the key's bit comes from its record name instead: KEY1-8 = SF Rush key n,
//   bit n - 1; KEYS0-7 / KEYG0-7 = Rush 2049 silver / gold coin, bit 0-7 / 8-15.
// - func_8005F00C (has every racer taken the key? Taken keys aren't shown) and func_8005F418 (take it) use this
//   file's masks, so nothing reaches the host's masks or the Controller Pak.
// - Coins are drawn with Rush 2049's coin models (merged into the converted geometry with behaviour 8): the placement
//   walker's model lookup is redirected to them, and the per-frame breakable update, which sets a breakable's model
//   from its class (KEYO1), keeps a coin's own model. SF Rush keys get the same from src/track1.cpp's record models
//   (Rush 1's KEYL1).
// - Coins are taken as in Rush 2049, by a car within 9 units of the coin (func_8008B0CC), not by Rush 2's key test,
//   which wants a key in the car's footprint and within about 2 units of its height. SF Rush keys get Rush 1's
//   breakable test from src/track1.cpp.
// - A coin plays Rush 2049's coin sound (0x06) when it is first taken.
// Rush 2's own keys and Dew cans aren't touched: once per frame (rush2_collect_frame) their masks are copied from the
// Controller Pak image's profiles, the race players' records and the no-profile table, for the Progress tab.
// Masks are kept per profile name in collectibles.json in the app folder. Players without a profile share one
// session-only set, like Rush 2's no-profile keys. As in Rush 2, keys are only placed with one player (the placement
// walker skips class records when racing with more).
// What they buy (src/unlocks.cpp) is kept with them: each profile's purchases, by item id, in the same file.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "json/json.hpp"

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"
#include "rush2_hooks.h"
#include "audio2049.h"
#include "collectibles.h"
#include "track1.h"
#include "track2049.h"

namespace {
    using namespace rush2::collectibles;

    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t race_players = 0x8010C3E2;   // s16: players in the race.
    constexpr uint32_t game_mode = 0x8010C3E8;      // Rush 2 takes no key in mode 3.
    constexpr uint32_t attract_demo = 0x800FAE6C;   // u8: nonzero while the attract demo runs (every key counts as taken).
    constexpr uint32_t player_slots = 0x800C2140;   // Per player, 0x28 bytes: +1 profile slot, +0x24 record pointer.
    constexpr uint32_t player_slot_size = 0x28;
    constexpr uint32_t profile_of_slot = 0x801174F0;    // s16 per profile slot: -1 = no profile.
    constexpr uint32_t current_record = 0x800D5790;     // The placement record being instantiated.
    constexpr uint32_t pak_records = 0x8004B220;        // 4 paks of 0x2200 bytes: 5 player records of 0x6C0 each.
    constexpr uint32_t pak_stride = 0x2200;
    constexpr uint32_t record_size = 0x6C0;
    constexpr int records_per_pak = 5;
    constexpr int profiles = 4 * records_per_pak;
    constexpr uint32_t rec_flags = 0x01;            // Bit 0: the record is in use.
    constexpr uint32_t rec_name = 0x02;
    constexpr uint32_t rec_keys = 0x18;             // Rush 2's key masks, u16 per track.
    constexpr uint32_t no_profile_keys = 0x800C1DBC;    // Rush 2's key masks of players without a profile (session).
    constexpr int local_players = 2;
    constexpr int name_size = 13;
    constexpr int coin_sound = 0x06;    // Rush 2049's coin pickup.
    // Rush 2049 takes a coin when the car is within its type's radius (type table 0x80117530 + 0x18: 9.0 for both
    // coins). Rush 2's key test wants the key in the car's footprint and within about 2 units of its height, which
    // would make 2049's coins, many of them floating over jumps, very hard to take.
    constexpr float coin_radius = 9.0f;
    constexpr uint32_t cars = 0x801124A0;           // Car state, 0x354 each: position first.
    constexpr uint32_t car_size = 0x354;

    // Courses: SF Rush tracks, then 2049 race tracks, then stunt arenas.
    constexpr int first_rush2049 = sfrush_courses;
    constexpr int first_stunt = first_rush2049 + rush2049_courses;
    constexpr int courses = first_stunt + stunt2049_courses;
    using Masks = std::array<uint16_t, courses>;

    std::mutex collect_mutex;
    std::map<std::string, Masks> store;     // By profile name, as in collectibles.json.
    Masks guest{};                          // Players without a profile, this session.
    std::map<std::string, std::set<std::string>> bought;   // Unlock system purchases by profile name.
    std::set<std::string> guest_bought;
    bool store_loaded = false;

    // Coin models in RDRAM for the placement walker's lookup: gold, then silver.
    const char* const coin_models[2] = { "GOLDCOING_COIN", "SILVERCOINS_COI" };
    uint32_t coin_names = 0;
    // Coins of the race being set up or raced (breakable address -> the model it was created with), for the per-frame
    // breakable update. Filled at creation, completed by the first update.
    struct Coin {
        bool seen = false;
        uint32_t node = 0;
        int16_t id = 0;
        uint16_t model = 0;
    };
    std::map<uint32_t, Coin> coins;

    // Rush 2's key masks, copied once per frame.
    using Rush2Masks = std::array<uint16_t, rush2_courses>;
    struct Rush2Snapshot {
        std::vector<std::pair<std::string, Rush2Masks>> profiles;   // In-use pak profiles, in pak order.
        Rush2Masks guest{};
    };
    Rush2Snapshot rush2_snapshot;

    // The course of the added track being raced, or -1.
    int hosted_course(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id);
        if (t == rush2::track2049::host_slot) {
            int k = rush2::track1::race_track();
            if (k >= 1 && k <= sfrush_courses) {
                return k - 1;
            }
            k = rush2::track2049::race_track();
            if (k >= 1 && k <= rush2049_courses) {
                return first_rush2049 + k - 1;
            }
        }
        else if (t == rush2::track2049::stunt_host_slot) {
            int n = rush2::track2049::stunt_arena();
            if (n >= 1 && n <= stunt2049_courses) {
                return first_stunt + n - 1;
            }
        }
        return -1;
    }

    std::string read_name(uint8_t* rdram, uint32_t addr, int size) {
        std::string s;
        for (int i = 0; i < size; i++) {
            uint8_t c = MEM_BU(0, (int32_t)(addr + i));
            if (c == 0) {
                break;
            }
            s.push_back((char)c);
        }
        return s;
    }

    // The bit of a key record on course c: KEY1-8 on an SF Rush track, KEYS0-7 / KEYG0-7 on a 2049 one; else -1.
    int bit_of(const std::string& name, int c) {
        if (name.size() < 4 || name.compare(0, 3, "KEY") != 0) {
            return -1;
        }
        if (c < first_rush2049) {
            return name.size() == 4 && name[3] >= '1' && name[3] <= '8' ? name[3] - '1' : -1;
        }
        if (name.size() == 5 && (name[3] == 'S' || name[3] == 'G') && name[4] >= '0' && name[4] < '0' + coins_per_kind) {
            return (name[3] == 'G' ? coins_per_kind : 0) + name[4] - '0';
        }
        return -1;
    }

    // Side file

    std::filesystem::path store_path() {
        return recompui::file::get_app_folder_path() / "collectibles.json";
    }

    std::string to_hex(const std::string& s) {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (unsigned char c : s) {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
        return out;
    }

    bool from_hex(const std::string& s, std::string& out) {
        auto digit = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        if (s.size() % 2 != 0) {
            return false;
        }
        out.clear();
        for (size_t i = 0; i < s.size(); i += 2) {
            int hi = digit(s[i]), lo = digit(s[i + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out.push_back((char)(hi << 4 | lo));
        }
        return true;
    }

    std::string printable(const std::string& name) {
        std::string s;
        for (char c : name) {
            s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return s;
    }

    // collectibles.json: { "version": 1, "profiles": [ { "name": "BOB", "key": "424f42", "sfrush": [7 masks],
    // "rush2049": [6 masks], "stunt2049": [4 masks], "unlocks": [item ids] } ] }. key is the name's bytes (the game's
    // character set isn't always ASCII); name is only for reading.
    struct Section {
        const char* key;
        int first;
        int count;
    };
    constexpr Section sections[] = { { "sfrush", 0, sfrush_courses }, { "rush2049", first_rush2049, rush2049_courses },
                                     { "stunt2049", first_stunt, stunt2049_courses } };

    void load_store() {
        if (store_loaded) {
            return;
        }
        store_loaded = true;
        std::ifstream f(store_path());
        if (!f) {
            return;
        }
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.contains("profiles") || !j["profiles"].is_array()) {
            printf("[collectibles] Couldn't read %s\n", store_path().string().c_str());
            return;
        }
        for (const auto& pj : j["profiles"]) {
            std::string name;
            if (!pj.is_object() || !pj.contains("key") || !pj["key"].is_string() ||
                !from_hex(pj["key"].get<std::string>(), name) || name.empty()) {
                continue;
            }
            Masks& m = store[name];
            if (pj.contains("unlocks") && pj["unlocks"].is_array()) {
                for (const auto& id : pj["unlocks"]) {
                    if (id.is_string()) {
                        bought[name].insert(id.get<std::string>());
                    }
                }
            }
            for (const Section& s : sections) {
                if (!pj.contains(s.key) || !pj[s.key].is_array()) {
                    continue;
                }
                const auto& list = pj[s.key];
                for (int i = 0; i < s.count && i < (int)list.size(); i++) {
                    if (list[i].is_number_unsigned()) {
                        m[s.first + i] = (uint16_t)list[i].get<uint32_t>();
                    }
                }
            }
        }
    }

    void save_store() {
        nlohmann::json list = nlohmann::json::array();
        std::set<std::string> names;
        for (const auto& [name, m] : store) {
            if (m != Masks{}) names.insert(name);
        }
        for (const auto& [name, ids] : bought) {
            if (!ids.empty()) names.insert(name);
        }
        for (const std::string& name : names) {
            auto it = store.find(name);
            Masks m = it != store.end() ? it->second : Masks{};
            nlohmann::json pj = { { "name", printable(name) }, { "key", to_hex(name) } };
            for (const Section& s : sections) {
                pj[s.key] = std::vector<uint16_t>(m.begin() + s.first, m.begin() + s.first + s.count);
            }
            auto ids = bought.find(name);
            if (ids != bought.end() && !ids->second.empty()) {
                pj["unlocks"] = std::vector<std::string>(ids->second.begin(), ids->second.end());
            }
            list.push_back(pj);
        }
        std::filesystem::path path = store_path();
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::trunc);
            if (!f) {
                printf("[collectibles] Couldn't write %s\n", temp.string().c_str());
                return;
            }
            f << nlohmann::json{ { "version", 1 }, { "profiles", list } }.dump(2) << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            printf("[collectibles] Couldn't replace %s: %s\n", path.string().c_str(), ec.message().c_str());
        }
    }

    // A player's profile name, or "" without a profile.
    std::string name_of_player(uint8_t* rdram, int player) {
        uint32_t slot = player_slots + (uint32_t)player * player_slot_size;
        uint8_t profile_slot = MEM_BU(0, (int32_t)(slot + 1));
        if (profile_slot >= profiles || (int16_t)MEM_H(0, (int32_t)(profile_of_slot + profile_slot * 2)) == -1) {
            return "";
        }
        uint32_t record = (uint32_t)MEM_W(0, (int32_t)(slot + 0x24));
        if (record < 0x80000000 || record >= 0x80800000) {
            return "";
        }
        return read_name(rdram, record + rec_name, name_size);
    }

    // A race player's masks: its profile's (by name), or the no-profile set. profile_name gets the name, or "".
    Masks& masks_of(uint8_t* rdram, int player, std::string& profile_name) {
        profile_name = name_of_player(rdram, player);
        if (profile_name.empty()) {
            return guest;
        }
        load_store();
        return store[profile_name];
    }

    int bits(uint32_t v) {
        int n = 0;
        for (; v != 0; v &= v - 1) n++;
        return n;
    }

    // Points: 1 per key and silver coin, 2 per Dew can and gold coin.
    int points_of(const Rush2Masks& r2, const Masks& m) {
        int p = 0;
        for (uint16_t mask : r2) {
            p += bits(mask & rush2_key_bits) + 2 * bits(mask & rush2_can_bits);
        }
        for (int c = 0; c < first_rush2049; c++) {
            p += bits(m[c] & ((1u << sfrush_keys[c]) - 1));
        }
        for (int c = first_rush2049; c < courses; c++) {
            p += bits(m[c] & silver_bits) + 2 * bits(m[c] & gold_bits);
        }
        return p;
    }

    // Rush 2's masks of a profile (or of players without one) from the last frame's copy.
    Rush2Masks rush2_masks_of(const std::string& name) {
        if (name.empty()) {
            return rush2_snapshot.guest;
        }
        for (const auto& [listed, masks] : rush2_snapshot.profiles) {
            if (listed == name) {
                return masks;
            }
        }
        return Rush2Masks{};
    }

    Masks side_masks_of(const std::string& name) {
        if (name.empty()) {
            return guest;
        }
        auto it = store.find(name);
        return it != store.end() ? it->second : Masks{};
    }

    Progress progress_of(const std::string& name, const Rush2Masks& r2, const Masks& m) {
        Progress p;
        p.name = name;
        p.rush2 = r2;
        std::copy(m.begin(), m.begin() + sfrush_courses, p.sfrush.begin());
        std::copy(m.begin() + first_rush2049, m.begin() + first_stunt, p.rush2049.begin());
        std::copy(m.begin() + first_stunt, m.end(), p.stunt2049.begin());
        return p;
    }
}

std::vector<Progress> rush2::collectibles::progress() {
    std::lock_guard lock{ collect_mutex };
    load_store();
    std::vector<Progress> out;
    auto add = [&](const std::string& name, const Rush2Masks& r2, const Masks& m) {
        out.push_back(progress_of(name, r2, m));
    };
    for (const auto& [name, r2] : rush2_snapshot.profiles) {
        auto it = store.find(name);
        add(name, r2, it != store.end() ? it->second : Masks{});
    }
    for (const auto& [name, m] : store) {
        bool listed = std::any_of(rush2_snapshot.profiles.begin(), rush2_snapshot.profiles.end(),
                                  [&](const auto& e) { return e.first == name; });
        if (!listed && m != Masks{}) {
            add(name, Rush2Masks{}, m);
        }
    }
    add("", rush2_snapshot.guest, guest);
    return out;
}

void rush2::collectibles::clear_profile(uint8_t* rdram, int p) {
    if (p < 0 || p >= profiles) {
        return;
    }
    uint32_t record = pak_records + (p / records_per_pak) * pak_stride + (p % records_per_pak) * record_size;
    std::string name = read_name(rdram, record + rec_name, name_size);
    std::lock_guard lock{ collect_mutex };
    load_store();
    bool erased = !name.empty() && store.erase(name) != 0;
    erased = (!name.empty() && bought.erase(name) != 0) || erased;
    if (erased) {
        save_store();
    }
}

std::string rush2::collectibles::player_name(uint8_t* rdram, int player) {
    return name_of_player(rdram, player);
}

Progress rush2::collectibles::player_progress(uint8_t* rdram, int player) {
    std::lock_guard lock{ collect_mutex };
    std::string name;
    Masks m = masks_of(rdram, player, name);
    return progress_of(name, rush2_masks_of(name), m);
}

int rush2::collectibles::points(const std::string& name) {
    std::lock_guard lock{ collect_mutex };
    load_store();
    return points_of(rush2_masks_of(name), side_masks_of(name));
}

int rush2::collectibles::max_points() {
    Rush2Masks r2;
    r2.fill(0xFFFF);
    Masks m;
    m.fill(0xFFFF);
    return points_of(r2, m);
}

std::set<std::string> rush2::collectibles::purchases(const std::string& name) {
    std::lock_guard lock{ collect_mutex };
    load_store();
    if (name.empty()) {
        return guest_bought;
    }
    auto it = bought.find(name);
    return it != bought.end() ? it->second : std::set<std::string>{};
}

void rush2::collectibles::reset_purchases(const std::string& name) {
    std::lock_guard lock{ collect_mutex };
    load_store();
    if (name.empty()) {
        guest_bought.clear();
    }
    else if (bought.erase(name) != 0) {
        save_store();
    }
}

bool rush2::collectibles::purchase(const std::string& name, const std::string& id, int cost, int spent) {
    std::lock_guard lock{ collect_mutex };
    load_store();
    std::set<std::string>& ids = name.empty() ? guest_bought : bought[name];
    if (ids.contains(id) || points_of(rush2_masks_of(name), side_masks_of(name)) - spent < cost) {
        return false;
    }
    ids.insert(id);
    if (!name.empty()) {
        save_store();
    }
    return true;
}

// func_800B0228 at 0x800B02B4, the top of the game thread's main loop (every frame): copies Rush 2's key masks for
// the Progress tab. A race player's record (player slot + 0x24) is read too, in case it is newer than the pak image's.
extern "C" void rush2_collect_frame(uint8_t* rdram, recomp_context* ctx) {
    Rush2Snapshot snap;
    auto masks_at = [&](uint32_t record) {
        Rush2Masks m;
        for (int t = 0; t < rush2_courses; t++) {
            m[t] = MEM_HU(0, (int32_t)(record + rec_keys + t * 2));
        }
        return m;
    };
    for (int p = 0; p < profiles; p++) {
        uint32_t record = pak_records + (p / records_per_pak) * pak_stride + (p % records_per_pak) * record_size;
        std::string name = read_name(rdram, record + rec_name, name_size);
        bool listed = std::any_of(snap.profiles.begin(), snap.profiles.end(), [&](const auto& e) { return e.first == name; });
        if ((MEM_BU(0, (int32_t)(record + rec_flags)) & 1) != 0 && !name.empty() && !listed) {
            snap.profiles.emplace_back(name, masks_at(record));
        }
    }
    for (int player = 0; player < local_players; player++) {
        uint32_t slot = player_slots + (uint32_t)player * player_slot_size;
        uint8_t profile_slot = MEM_BU(0, (int32_t)(slot + 1));
        uint32_t record = (uint32_t)MEM_W(0, (int32_t)(slot + 0x24));
        if ((int16_t)MEM_H(0, (int32_t)(profile_of_slot + profile_slot * 2)) == -1 || record < 0x80000000 ||
            record >= 0x80800000) {
            continue;
        }
        std::string name = read_name(rdram, record + rec_name, name_size);
        for (auto& [listed, masks] : snap.profiles) {
            if (listed == name) {
                masks = masks_at(record);
            }
        }
    }
    for (int t = 0; t < rush2_courses; t++) {
        snap.guest[t] = MEM_HU(0, (int32_t)(no_profile_keys + t * 2));
    }
    std::lock_guard lock{ collect_mutex };
    rush2_snapshot = std::move(snap);
}

// Start of func_8008BC64 (the race's object setup, before the placement is instantiated): a new race's coins.
extern "C" void rush2_collect_race_reset(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ collect_mutex };
    coins.clear();
}

// func_800BC3F8 at 0x800BC490 ($v1 = the new key, its number at +0x68): on an added track, the bit named by its record.
extern "C" void rush2_collect_number(uint8_t* rdram, recomp_context* ctx) {
    int c = hosted_course(rdram);
    if (c < 0) {
        return;
    }
    uint32_t key = (uint32_t)ctx->r3;
    std::string name = read_name(rdram, (uint32_t)MEM_W(0, (int32_t)current_record), 16);
    int bit = bit_of(name, c);
    if (bit < 0) {
        return;
    }
    MEM_H(0, (int32_t)(key + 0x68)) = (int16_t)bit;
    if (c >= first_rush2049) {
        // The radius of the hit test's first, sphere check (func_8008B95C).
        uint32_t radius;
        std::memcpy(&radius, &coin_radius, sizeof(radius));
        MEM_W(0, (int32_t)(key + 0x54)) = (int32_t)radius;
        std::lock_guard lock{ collect_mutex };
        coins[key] = Coin{};
    }
}

// Start of func_8005F00C ($a0 = key bit): returns ($v0) whether every racer has taken the key. On an added track the
// answer comes from this file's masks.
extern "C" int rush2_collect_taken(uint8_t* rdram, recomp_context* ctx) {
    int c = hosted_course(rdram);
    if (c < 0) {
        return 0;
    }
    if (MEM_BU(0, (int32_t)attract_demo) != 0) {
        ctx->r2 = 1;
        return 1;
    }
    uint32_t bit = (uint32_t)ctx->r4 & 31;
    int players = (int16_t)MEM_H(0, (int32_t)race_players);
    std::lock_guard lock{ collect_mutex };
    std::string name;
    for (int p = 0; p < players; p++) {
        if ((masks_of(rdram, p, name)[c] >> bit & 1) == 0) {
            ctx->r2 = 0;
            return 1;
        }
    }
    ctx->r2 = 1;
    return 1;
}

// Start of func_8005F418 ($a0 = key bit, $a1 = player): the player takes the key. On an added track it goes into this
// file's masks (saved at once for a profile).
extern "C" int rush2_collect_take(uint8_t* rdram, recomp_context* ctx) {
    int c = hosted_course(rdram);
    if (c < 0) {
        return 0;
    }
    if (MEM_BU(0, (int32_t)attract_demo) != 0 || MEM_W(0, (int32_t)game_mode) == 3) {
        return 1;
    }
    uint32_t bit = (uint32_t)ctx->r4 & 31;
    int player = (int32_t)ctx->r5;
    bool taken;
    {
        std::lock_guard lock{ collect_mutex };
        std::string name;
        uint16_t& mask = masks_of(rdram, player, name)[c];
        uint16_t before = mask;
        mask |= (uint16_t)(1u << bit);
        taken = mask != before;
        if (taken && !name.empty()) {
            save_store();
        }
    }
    if (taken && c >= first_rush2049 && rush2::audio2049::loaded()) {
        rush2::audio2049::sfx_start(coin_sound);
    }
    return 1;
}

// Start of func_8008B0CC, the car hit test for a breakable ($a0 = the car, $a1 = the breakable; returns $v0): a coin is
// taken as in Rush 2049, by a car within coin_radius of it.
extern "C" int rush2_collect_hit(uint8_t* rdram, recomp_context* ctx) {
    uint32_t breakable = (uint32_t)ctx->r5;
    {
        std::lock_guard lock{ collect_mutex };
        if (!coins.contains(breakable)) {
            return 0;
        }
    }
    auto f = [&](uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float v;
        std::memcpy(&v, &w, sizeof(v));
        return v;
    };
    uint32_t car = cars + (int16_t)ctx->r4 * car_size;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = f(breakable + 0x2C + i * 4) - f(car + i * 4);
        d2 += d * d;
    }
    ctx->r2 = d2 < coin_radius * coin_radius;
    return 1;
}

// func_80081790 at 0x8008194C, the placement walker's model lookup ($a0 = the class's model, KEYO1 for a key): a coin
// draws Rush 2049's coin model.
extern "C" void rush2_collect_record_model(uint8_t* rdram, recomp_context* ctx) {
    int c = hosted_course(rdram);
    if (c < first_rush2049) {
        return;
    }
    int bit = bit_of(read_name(rdram, (uint32_t)MEM_W(0, (int32_t)current_record), 16), c);
    if (bit < 0) {
        return;
    }
    if (coin_names == 0) {
        coin_names = (uint32_t)((uint8_t*)recomp::alloc(rdram, 32) - rdram) + 0x80000000;
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 16; i++) {
                size_t n = strlen(coin_models[k]);
                MEM_B(0, (int32_t)(coin_names + k * 16 + i)) = i < (int)n ? coin_models[k][i] : 0;
            }
        }
    }
    ctx->r4 = (int32_t)(coin_names + (bit >= coins_per_kind ? 0 : 16));
}

// func_8008A01C at 0x8008A0CC, the per-frame breakable update ($s0 = the breakable, $t3 = its node, $a0 = the model
// about to be set from the breakable's +0x62 id, KEYO1 for a key): a coin keeps the model it was created with while
// the id is the one it started with.
extern "C" void rush2_collect_breakable_model(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ collect_mutex };
    if (coins.empty()) {
        return;
    }
    uint32_t breakable = (uint32_t)ctx->r16;
    auto it = coins.find(breakable);
    if (it == coins.end()) {
        return;
    }
    uint32_t node = (uint32_t)ctx->r11;
    Coin& coin = it->second;
    if (!coin.seen) {
        coin = Coin{ true, node, (int16_t)MEM_H(0, (int32_t)(breakable + 0x62)), (uint16_t)MEM_HU(0, (int32_t)(node + 0xC)) };
    }
    if (coin.node == node && coin.id == (int16_t)MEM_H(0, (int32_t)(breakable + 0x62))) {
        ctx->r4 = coin.model;
    }
}
