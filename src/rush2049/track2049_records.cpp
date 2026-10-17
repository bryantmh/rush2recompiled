// Records for the Rush 2049 and SF Rush tracks (docs/rush2049_research/records.md).
//
// Both kinds of added track race in the host slot, and so does Rush 2049's obstacle course; below, "2049 track" stands
// for any of them. Internally the added tracks are numbered 1-6 (Rush 2049), 7-13 (SF Rush tracks 1-7) and 14 (the
// obstacle course); the save file's "records" section (include/data_files.h) keeps the SF Rush ones as
// { "game": "sfrush", "track": 1-7 } and the
// obstacle course as { "track": "obstacle" }, so files written before they existed still load.
//
// A 2049 track is raced in the host slot (HAWAII, src/rush2049/track2049.cpp), and Rush 2 indexes its records by track, so
// without this file a 2049 race would read and write HAWAII's records. Rush 2 keeps three kinds of records, at
// record slot s = track + 12 * backward (minus 1 from 11 up):
// - Stats (0x20 bytes per slot: races, places, average lap, ...) in each player record of the Controller Pak image,
//   record + 0x1F0 + s * 0x20, or in the no-profile table 0x800C1DD4 (index track + 12 * backward, RAM only).
// - Each profile's five best times per page (page 0: the race's average lap, page 1: its best lap), also in the
//   Controller Pak image at record + 0x38 + page * 0xDC + s * 0xA.
// - The high-score table the post-race and pause screens show (0x800D2160, 2 pages x 22 slots x 0x12 bytes: five
//   "who" bytes and five times). It isn't saved: the game merges every profile's best times into it, and players
//   without a profile add guest entries whose names live in 0x800D2630 until the console is switched off.
//
// The Controller Pak image must never hold 2049 values, so:
// - Stats and best times: every place that computes a HAWAII slot address while a 2049 track is hosted gets it
//   redirected to a side buffer in recomp memory (one block per player-record position, holding the 12 forward and
//   backward 2049 tracks), which the game then reads and writes in place. The game's dirty marking ignores addresses
//   outside the pak image, so nothing reaches the .mpk. Blocks are bound to a profile by name; they are loaded from
//   and saved to the save file's "records" section. The no-profile stats go to a session-only block, as
//   Rush 2's own do.
// - High-score table: the table isn't saved, so while the two functions that use it in a race run (func_800A952C,
//   post-race records and name entry, and func_800606A8, the high-score screen) HAWAII's four rows (2 pages x
//   forward/backward) are swapped for the 2049 track's, and swapped back when they return. The 2049 rows are built
//   the way the game builds its own: the guest entries kept from earlier in the session, plus every profile's best
//   times from its side block. Guest names are kept as text while a row is swapped out, since the game reuses
//   name entries it thinks are unreferenced.
// - The high-score screen fills empty rows with made-up times from the track's seed time (0x8001CF3C); for a 2049
//   track the seed comes from Rush 2049's own table (2049 boot 0x8002E870).
//
// Custom tracks (src/track_mod/) race in the host slot too and get one more course, numbered 15 below, which holds
// the raced custom track's records: the save file keeps every custom track's as { "mod": "<its folder>" }, and when
// another one is raced the course is swapped (sync_mod). The high-score screen's seed is the track's best_times.
//
// The records screen in the menus lists the added courses after Rush 2's (screen_entries): while one is shown the
// screen's index stays on HAWAII's, HAWAII's logo container holds the course's logo, and the list's rows, best times,
// stats and seed move to the course's as in a race. Custom tracks aren't listed. 2049 results don't count toward
// Rush 2's unlocks. Clearing a profile's records there, or deleting the profile, clears its 2049 records as well.
//
// Rush 2049's stunt arenas are hosted by STUNT1 (track 11), which has no lap records, only stats (the stunt counters
// and best scores of its stats page). They are numbered 16-19 below and get the same treatment with STUNT1's slots:
// the stunt stats writers' addresses move to the arena's side-block stats during a session in one, and the records
// screen lists the arenas on its stats page after the other added courses, on STUNT1's index. The save file keeps
// them as { "track": "stunt1".."stunt4" }.
//
// California Speed's tracks (src/calspeed/) race in the host slot like the SF Rush ones and are courses 20-35 below,
// forward only (California Speed has no backward races); the save file keeps them as { "game": "calspeed",
// "track": 1-16 }. Their seed is the track's AI lane 0 time over a lap (rush2::calspeed::record_seed).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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

#include "data_files.h"
#include "rush2_hooks.h"
#include "collectibles.h"
#include "track1.h"
#include "calspeed.h"
#include "track2049.h"
#include "track_mod_runtime.h"
#include "unlocks.h"
#include "wings.h"

using rush2::track2049::host_slot;
using rush2::track2049::track_count;

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t backward_flag = 0x80119848;

    // The Controller Pak image: 4 paks of 0x2200 bytes, each a 0x40-byte header and 5 player records of 0x6C0.
    constexpr uint32_t pak_records = 0x8004B220;
    constexpr uint32_t pak_stride = 0x2200;
    constexpr uint32_t record_size = 0x6C0;
    constexpr int records_per_pak = 5;
    constexpr int profiles = 4 * records_per_pak;   // Profile index = pak * 5 + record (the high-score "who").
    constexpr uint32_t rec_flags = 0x01;            // Bit 0: the record is in use.
    constexpr uint32_t rec_name = 0x02;
    constexpr uint32_t rec_times = 0x38;            // [2 pages][22 slots][5] packed times.
    constexpr uint32_t rec_stats = 0x1F0;           // [23 slots] stats.
    constexpr uint32_t page_size = 0xDC;
    constexpr uint32_t list_size = 0xA;
    constexpr uint32_t stats_size = 0x20;

    constexpr uint32_t no_profile_stats = 0x800C1DD4; // [24] stats, index track + 12 * backward.
    constexpr uint32_t scores = 0x800D2160;           // [2 pages][22 slots] high-score rows.
    constexpr uint32_t score_page = 0x18C;
    constexpr uint32_t row_size = 0x12;               // who[5], pad, u16 time[5], laps, pad.
    constexpr uint32_t guest_names = 0x800D2630;      // [20] names of 13 bytes.
    constexpr uint32_t name_ages = 0x800D3AF8;        // [20] u16, frames since the name was last chosen.
    constexpr uint32_t seeds = 0x8001CF3C;            // f32[24], index track + 12 * backward.
    constexpr int name_size = 13;
    constexpr int guest_count = 20;
    constexpr int first_guest = 20;                   // who >= 20: guest name who - 20; 0xFF: empty.
    constexpr uint8_t empty = 0xFF;

    static_assert(host_slot < 11, "the host's backward slot is computed for tracks below 11");
    constexpr int host_record_slot[2] = { host_slot, host_slot + 12 - 1 };
    constexpr int host_no_profile_slot[2] = { host_slot, host_slot + 12 };
    // STUNT1's, which hosts the stunt arenas: its record slot is 10, MIDWAY's as well (the game takes 1 off from 11
    // up), so only the track tells them apart.
    constexpr int stunt_host = rush2::track2049::stunt_host_slot;
    static_assert(stunt_host == 11, "the stunt host's record slot is computed for track 11");
    constexpr int stunt_record_slot[2] = { stunt_host - 1, stunt_host + 12 - 1 };
    constexpr int stunt_no_profile_slot[2] = { stunt_host, stunt_host + 12 };

    // The added tracks forward and backward: course = (track - 1) + 14 * backward.
    constexpr int r1_count = rush2::track1::track_count;
    constexpr int obstacle_track = track_count + r1_count + 1;     // The obstacle course (forward only).
    constexpr int mod_track = obstacle_track + 1;                  // The raced custom track.
    constexpr int stunt_count = rush2::track2049::stunt_count;
    constexpr int stunt_track = mod_track + 1;                     // Stunt arena 1 (stats only, forward only).
    constexpr int cs_count = rush2::calspeed::track_count;
    constexpr int cs_track = stunt_track + stunt_count;            // Cal Speed track 0 (forward only).
    constexpr int extra_tracks = cs_track + cs_count - 1;
    constexpr int courses = extra_tracks * 2;

    // A side block in RDRAM per player-record position: the stats of each course, then its two pages of times.
    constexpr uint32_t block_times = courses * stats_size;
    constexpr uint32_t block_size = block_times + courses * 2 * list_size;

    // Rush 2049's seed times for its race tracks (boot segment 0x8002E870, f32, index track + 19 * backward), used
    // when the ROM can't be read.
    constexpr uint32_t seeds_2049_vram = 0x8002E870;
    constexpr float seeds_2049[2][track_count] = { { 47, 81, 65, 97, 78, 117 }, { 48, 79, 76, 104, 86, 119 } };
    constexpr float obstacle_seed = 68;

    struct Course {
        uint8_t stats[stats_size] = {};
        uint16_t times[2][5] = {};

        bool operator==(const Course& o) const {
            return std::memcmp(stats, o.stats, sizeof(stats)) == 0 && std::memcmp(times, o.times, sizeof(times)) == 0;
        }
        bool zero() const { return *this == Course{}; }
    };
    using Profile = std::array<Course, courses>;

    // A high-score row, with the names of its guest entries.
    struct Row {
        uint8_t raw[row_size] = { empty, empty, empty, empty, empty };
        std::string guest[5];
    };

    std::mutex records_mutex;
    std::map<std::string, Profile> store;   // By profile name, as in the save file.
    bool store_loaded = false;
    bool store_dirty = false;

    uint32_t blocks = 0;        // RDRAM: one block per position, then the no-profile stats.
    uint32_t no_profile_block = 0;
    uint32_t all_stats_block = 0;   // The records screen's ALL stats of the course it shows (screen_all_stats).
    std::string bound_name[profiles];
    bool bound[profiles] = {};
    Profile bound_copy[profiles];  // The block as last loaded or saved.
    bool touched = false;          // A side block was handed to the game since the last save.

    Row session_rows[courses][2]; // The 2049 high-score rows while swapped out (guests; profiles are re-merged).

    // Custom tracks' records by profile name and folder, [backward]; the mod_track course of store and of the blocks
    // holds loaded_mod's.
    using ModCourses = std::array<Course, 2>;
    std::map<std::string, std::map<std::string, ModCourses>> mod_store;
    std::map<std::string, std::array<std::array<Row, 2>, 2>> mod_rows;   // session rows by folder, [backward][page]
    std::string loaded_mod;
    Row host_rows[2][2];          // HAWAII's rows while the 2049 ones are swapped in, [backward][page].
    int depth = 0;
    int swapped = 0;              // The 2049 track whose rows are swapped in, or 0.

    int course_of(int k, int backward) {
        return (k - 1) + extra_tracks * backward;
    }

    bool is_stunt(int k) {
        return k >= stunt_track && k < stunt_track + stunt_count;
    }

    bool is_cs(int k) {
        return k >= cs_track && k < cs_track + cs_count;
    }

    // The stunt arena hosted by the current session in STUNT1, as an added track number, or 0.
    int hosted_stunt(uint8_t* rdram) {
        int n = rush2::track2049::stunt_arena();
        return MEM_B(0, (int32_t)track_id) == stunt_host && n >= 1 && n <= stunt_count ? stunt_track + n - 1 : 0;
    }

    // The added track hosted by the current race (1-6 Rush 2049, 7-13 SF Rush, 14 the obstacle course, 15 the custom
    // track, 20-35 Cal Speed), or 0.
    int hosted(uint8_t* rdram) {
        if (MEM_B(0, (int32_t)track_id) != host_slot) {
            return 0;
        }
        int k = rush2::track2049::race_track();
        if (k >= 1 && k <= track_count) {
            return k;
        }
        if (k == rush2::track2049::obstacle) {
            return obstacle_track;
        }
        int r = rush2::track1::race_track();
        if (r >= 1 && r <= r1_count) {
            return track_count + r;
        }
        if (rush2::track_mod::race_track() >= 0) {
            return mod_track;
        }
        int c = rush2::calspeed::race_track();
        if (c >= 1 && c <= cs_count) {
            return cs_track + c - 1;
        }
        return 0;
    }

    uint32_t record(int p) {
        return pak_records + (p / records_per_pak) * pak_stride + (p % records_per_pak) * record_size;
    }

    uint32_t block(int p) {
        return blocks + p * block_size;
    }

    uint32_t times_in_block(int p, int course, int page) {
        return block(p) + block_times + (course * 2 + page) * list_size;
    }

    std::string read_name(uint8_t* rdram, uint32_t addr) {
        std::string s;
        for (int i = 0; i < name_size; i++) {
            uint8_t c = MEM_BU(0, (int32_t)(addr + i));
            if (c == 0) {
                break;
            }
            s.push_back((char)c);
        }
        return s;
    }

    void write_name(uint8_t* rdram, uint32_t addr, const std::string& s) {
        for (int i = 0; i < name_size; i++) {
            MEM_B(0, (int32_t)(addr + i)) = (i < name_size - 1 && i < (int)s.size()) ? s[i] : 0;
        }
    }

    // Save file section (include/data_files.h)

    const std::string records_section = "records";

    std::string format_time(uint16_t t) {
        char s[16];
        snprintf(s, sizeof(s), "%u:%02u.%02u", (t >> 13) & 7, (t >> 7) & 0x3F, t & 0x7F);
        return s;
    }

    uint16_t parse_time(const nlohmann::json& j) {
        if (!j.is_string()) {
            return 0;
        }
        std::string text = j.get<std::string>();
        char* end;
        unsigned long m = std::strtoul(text.c_str(), &end, 10);
        if (*end != ':') {
            return 0;
        }
        unsigned long s = std::strtoul(end + 1, &end, 10);
        if (*end != '.') {
            return 0;
        }
        unsigned long h = std::strtoul(end + 1, &end, 10);
        return uint16_t(((m & 7) << 13) | ((s & 0x3F) << 7) | (h & 0x7F));
    }

    std::string to_hex(const uint8_t* p, size_t n) {
        static const char digits[] = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; i++) {
            s.push_back(digits[p[i] >> 4]);
            s.push_back(digits[p[i] & 15]);
        }
        return s;
    }

    int hex_digit(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }

    bool from_hex(const std::string& s, uint8_t* p, size_t n) {
        if (s.size() != n * 2) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            int hi = hex_digit(s[i * 2]), lo = hex_digit(s[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            p[i] = uint8_t(hi << 4 | lo);
        }
        return true;
    }

    // Names use the game's character set, so the file keys profiles by the name's bytes in hex and shows a
    // readable copy beside it.
    std::string printable(const std::string& name) {
        std::string s;
        for (char c : name) {
            s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return s;
    }

    void read_course(const nlohmann::json& cj, Course& c) {
        if (cj.contains("stats") && cj["stats"].is_string()) {
            from_hex(cj["stats"].get<std::string>(), c.stats, sizeof(c.stats));
        }
        const char* pages[2] = { "race", "lap" };
        for (int page = 0; page < 2; page++) {
            if (cj.contains(pages[page]) && cj[pages[page]].is_array()) {
                const auto& list = cj[pages[page]];
                for (size_t i = 0; i < 5 && i < list.size(); i++) {
                    c.times[page][i] = parse_time(list[i]);
                }
            }
        }
    }

    nlohmann::json write_course(const Course& c) {
        nlohmann::json pages[2] = { nlohmann::json::array(), nlohmann::json::array() };
        for (int page = 0; page < 2; page++) {
            for (int i = 0; i < 5; i++) {
                pages[page].push_back(c.times[page][i] != 0 ? nlohmann::json(format_time(c.times[page][i]))
                                                             : nlohmann::json(nullptr));
            }
        }
        return { { "race", pages[0] }, { "lap", pages[1] }, { "stats", to_hex(c.stats, sizeof(c.stats)) } };
    }

    // The store's mod_track courses back into mod_store (they hold loaded_mod's records).
    void stash_mod() {
        if (loaded_mod.empty()) {
            return;
        }
        for (auto& [name, profile] : store) {
            ModCourses mc = { profile[course_of(mod_track, 0)], profile[course_of(mod_track, 1)] };
            if (!mc[0].zero() || !mc[1].zero() || mod_store[name].count(loaded_mod)) {
                mod_store[name][loaded_mod] = mc;
            }
        }
    }

    void load_store() {
        if (store_loaded) {
            return;
        }
        store_loaded = true;
        nlohmann::json j = nlohmann::json::parse(
            rush2::data_files::read(rush2::data_files::File::Saves, records_section), nullptr, false);
        if (j.is_discarded() || !j.contains("profiles") || !j["profiles"].is_array()) {
            return;
        }
        for (const auto& pj : j["profiles"]) {
            if (!pj.is_object() || !pj.contains("key") || !pj["key"].is_string() || !pj.contains("courses")) {
                continue;
            }
            std::string key_hex = pj["key"].get<std::string>();
            std::string name(key_hex.size() / 2, '\0');
            if (!from_hex(key_hex, (uint8_t*)name.data(), name.size())) {
                continue;
            }
            Profile& profile = store[name];
            for (const auto& cj : pj["courses"]) {
                if (!cj.is_object() || (!cj.contains("track") && !cj.contains("mod"))) {
                    continue;
                }
                int k;
                int backward = cj.value("backward", false) ? 1 : 0;
                if (cj.contains("mod") && cj["mod"].is_string()) {
                    Course& c = mod_store[name][cj["mod"].get<std::string>()][backward];
                    read_course(cj, c);
                    continue;
                }
                if (cj["track"].is_string() && cj["track"].get<std::string>() == "obstacle") {
                    k = obstacle_track;
                }
                else if (cj["track"].is_string()) {
                    std::string text = cj["track"].get<std::string>();
                    int n = text.size() == 6 && text.compare(0, 5, "stunt") == 0 ? text[5] - '0' : 0;
                    if (n < 1 || n > stunt_count) {
                        continue;
                    }
                    k = stunt_track + n - 1;
                }
                else if (cj["track"].is_number_integer()) {
                    k = cj["track"].get<int>();
                    std::string game = cj.contains("game") && cj["game"].is_string() ? cj["game"].get<std::string>() : "";
                    bool sfrush = game == "sfrush", calspeed = game == "calspeed";
                    if (k < 1 || k > (sfrush ? r1_count : calspeed ? cs_count : track_count)) {
                        continue;
                    }
                    if (sfrush) {
                        k += track_count;
                    }
                    else if (calspeed) {
                        k += cs_track - 1;
                    }
                }
                else {
                    continue;
                }
                read_course(cj, profile[course_of(k, backward)]);
            }
        }
    }

    void save_store() {
        stash_mod();
        nlohmann::json list = nlohmann::json::array();
        std::set<std::string> names;
        for (const auto& [name, profile] : store) names.insert(name);
        for (const auto& [name, mods] : mod_store) names.insert(name);
        for (const std::string& name : names) {
            nlohmann::json course_list = nlohmann::json::array();
            auto it = store.find(name);
            const Profile profile = it != store.end() ? it->second : Profile{};
            for (int course = 0; course < courses; course++) {
                const Course& c = profile[course];
                int k = course % extra_tracks + 1;
                if (c.zero() || k == mod_track) {
                    continue;
                }
                nlohmann::json entry = write_course(c);
                entry["track"] = k > track_count ? k - track_count : k;
                entry["backward"] = course >= extra_tracks;
                if (k == obstacle_track) {
                    entry["track"] = "obstacle";
                }
                else if (is_stunt(k)) {
                    entry["track"] = "stunt" + std::to_string(k - stunt_track + 1);
                }
                else if (is_cs(k)) {
                    entry["track"] = k - cs_track + 1;
                    entry["game"] = "calspeed";
                }
                else if (k > track_count) {
                    entry["game"] = "sfrush";
                }
                course_list.push_back(entry);
            }
            auto mods = mod_store.find(name);
            if (mods != mod_store.end()) {
                for (const auto& [folder, mc] : mods->second) {
                    for (int backward = 0; backward < 2; backward++) {
                        if (mc[backward].zero()) continue;
                        nlohmann::json entry = write_course(mc[backward]);
                        entry["mod"] = folder;
                        entry["backward"] = backward != 0;
                        course_list.push_back(entry);
                    }
                }
            }
            if (!course_list.empty()) {
                list.push_back({ { "name", printable(name) },
                                 { "key", to_hex((const uint8_t*)name.data(), name.size()) },
                                 { "courses", course_list } });
            }
        }
        if (!rush2::data_files::write(rush2::data_files::File::Saves, records_section,
                nlohmann::json{ { "version", 1 }, { "profiles", list } }.dump())) {
            return;
        }
        store_dirty = false;
    }

    // Side blocks

    Profile read_block(uint8_t* rdram, int p) {
        Profile profile;
        for (int course = 0; course < courses; course++) {
            Course& c = profile[course];
            for (uint32_t i = 0; i < stats_size; i++) {
                c.stats[i] = MEM_BU(0, (int32_t)(block(p) + course * stats_size + i));
            }
            for (int page = 0; page < 2; page++) {
                for (int i = 0; i < 5; i++) {
                    c.times[page][i] = MEM_HU(0, (int32_t)(times_in_block(p, course, page) + i * 2));
                }
            }
        }
        return profile;
    }

    void write_block(uint8_t* rdram, int p, const Profile& profile) {
        for (int course = 0; course < courses; course++) {
            const Course& c = profile[course];
            for (uint32_t i = 0; i < stats_size; i++) {
                MEM_B(0, (int32_t)(block(p) + course * stats_size + i)) = (int8_t)c.stats[i];
            }
            for (int page = 0; page < 2; page++) {
                for (int i = 0; i < 5; i++) {
                    MEM_H(0, (int32_t)(times_in_block(p, course, page) + i * 2)) = (int16_t)c.times[page][i];
                }
            }
        }
    }

    // Copies block p into the store if the game changed it. Other positions bound to the same name (the same name
    // on two paks) that the game hasn't changed are reloaded so they don't overwrite it later.
    void save_block(uint8_t* rdram, int p) {
        if (!bound[p]) {
            return;
        }
        Profile profile = read_block(rdram, p);
        if (profile == bound_copy[p]) {
            return;
        }
        store[bound_name[p]] = profile;
        bound_copy[p] = profile;
        store_dirty = true;
        for (int q = 0; q < profiles; q++) {
            if (q != p && bound[q] && bound_name[q] == bound_name[p] && read_block(rdram, q) == bound_copy[q]) {
                write_block(rdram, q, profile);
                bound_copy[q] = profile;
            }
        }
    }

    // Makes block p hold the records of the profile now in player-record position p.
    void bind(uint8_t* rdram, int p) {
        std::string name = read_name(rdram, record(p) + rec_name);
        if (bound[p] && bound_name[p] == name) {
            return;
        }
        save_block(rdram, p);
        auto it = store.find(name);
        Profile profile = it != store.end() ? it->second : Profile{};
        if (it == store.end() && !loaded_mod.empty()) {
            auto mods = mod_store.find(name);
            if (mods != mod_store.end() && mods->second.count(loaded_mod)) {
                profile[course_of(mod_track, 0)] = mods->second[loaded_mod][0];
                profile[course_of(mod_track, 1)] = mods->second[loaded_mod][1];
            }
        }
        write_block(rdram, p, profile);
        bound_name[p] = name;
        bound_copy[p] = profile;
        bound[p] = true;
    }

    void ensure_blocks(uint8_t* rdram) {
        if (blocks != 0) {
            return;
        }
        load_store();
        uint32_t size = profiles * block_size + courses * stats_size + stats_size;
        blocks = (uint32_t)((uint8_t*)recomp::alloc(rdram, size) - rdram) + 0x80000000;
        no_profile_block = blocks + profiles * block_size;
        all_stats_block = no_profile_block + courses * stats_size;
        for (uint32_t i = 0; i < size; i++) {
            MEM_B(0, (int32_t)(blocks + i)) = 0;
        }
    }

    // Makes the mod_track course (store, side blocks, session rows) hold the raced custom track's records.
    void sync_mod(uint8_t* rdram) {
        std::string folder = rush2::track_mod::race_folder();
        if (folder.empty() || folder == loaded_mod) {
            return;
        }
        for (int p = 0; p < profiles; p++) {
            save_block(rdram, p);
        }
        stash_mod();
        for (auto& [name, profile] : store) {
            ModCourses mc{};
            auto mods = mod_store.find(name);
            if (mods != mod_store.end() && mods->second.count(folder)) mc = mods->second[folder];
            profile[course_of(mod_track, 0)] = mc[0];
            profile[course_of(mod_track, 1)] = mc[1];
        }
        for (int p = 0; p < profiles; p++) {
            if (!bound[p]) continue;
            auto it = store.find(bound_name[p]);
            Profile profile = it != store.end() ? it->second : Profile{};
            if (it == store.end()) {
                ModCourses mc{};
                auto mods = mod_store.find(bound_name[p]);
                if (mods != mod_store.end() && mods->second.count(folder)) mc = mods->second[folder];
                profile[course_of(mod_track, 0)] = mc[0];
                profile[course_of(mod_track, 1)] = mc[1];
            }
            write_block(rdram, p, profile);
            bound_copy[p] = profile;
        }
        for (int backward = 0; backward < 2; backward++) {
            for (int page = 0; page < 2; page++) {
                if (!loaded_mod.empty()) mod_rows[loaded_mod][backward][page] = session_rows[course_of(mod_track, backward)][page];
                auto rows = mod_rows.find(folder);
                session_rows[course_of(mod_track, backward)][page] = rows != mod_rows.end() ? rows->second[backward][page] : Row{};
            }
        }
        loaded_mod = folder;
    }

    void save_changes(uint8_t* rdram) {
        if (!touched) {
            return;
        }
        touched = false;
        for (int p = 0; p < profiles; p++) {
            save_block(rdram, p);
        }
        if (store_dirty) {
            save_store();
        }
    }

    // Address redirection. Each returns addr unchanged unless it is one of HAWAII's slots and a 2049 track is hosted
    // (for the stats, also one of STUNT1's while it hosts a stunt arena).

    // A stats slot: record + 0x1F0 + slot * 0x20 in the pak image, or an entry of the no-profile table.
    uint32_t redirect_stats_to(uint8_t* rdram, uint32_t addr, int k) {
        const int* record_slot = is_stunt(k) ? stunt_record_slot : host_record_slot;
        const int* no_profile_slot = is_stunt(k) ? stunt_no_profile_slot : host_no_profile_slot;
        for (int backward = 0; backward < 2; backward++) {
            if (addr == no_profile_stats + no_profile_slot[backward] * stats_size) {
                touched = true;
                return no_profile_block + course_of(k, backward) * stats_size;
            }
        }
        uint32_t off = addr - pak_records;
        if (addr < pak_records || off >= 4 * pak_stride || off % pak_stride >= records_per_pak * record_size) {
            return addr;
        }
        int p = (off / pak_stride) * records_per_pak + (off % pak_stride) / record_size;
        uint32_t field = off % pak_stride % record_size;
        for (int backward = 0; backward < 2; backward++) {
            if (field == rec_stats + record_slot[backward] * stats_size) {
                bind(rdram, p);
                touched = true;
                return block(p) + course_of(k, backward) * stats_size;
            }
        }
        return addr;
    }

    uint32_t redirect_stats(uint8_t* rdram, uint32_t addr) {
        int k = hosted(rdram);
        if (k == 0) {
            k = hosted_stunt(rdram);
        }
        if (k == 0) {
            return addr;
        }
        ensure_blocks(rdram);
        sync_mod(rdram);
        return redirect_stats_to(rdram, addr, k);
    }

    // A best-times list: record + 0x38 + page * 0xDC + slot * 0xA in the pak image.
    uint32_t redirect_times_to(uint8_t* rdram, uint32_t addr, int k) {
        uint32_t off = addr - pak_records;
        if (addr < pak_records || off >= 4 * pak_stride || off % pak_stride >= records_per_pak * record_size) {
            return addr;
        }
        int p = (off / pak_stride) * records_per_pak + (off % pak_stride) / record_size;
        uint32_t field = off % pak_stride % record_size;
        for (int page = 0; page < 2; page++) {
            for (int backward = 0; backward < 2; backward++) {
                if (field == rec_times + page * page_size + host_record_slot[backward] * list_size) {
                    bind(rdram, p);
                    touched = true;
                    return times_in_block(p, course_of(k, backward), page);
                }
            }
        }
        return addr;
    }

    uint32_t redirect_times(uint8_t* rdram, uint32_t addr) {
        int k = hosted(rdram);
        if (k == 0) {
            return addr;
        }
        ensure_blocks(rdram);
        sync_mod(rdram);
        return redirect_times_to(rdram, addr, k);
    }

    // High-score rows

    uint32_t row_address(int page, int backward) {
        return scores + page * score_page + host_record_slot[backward] * row_size;
    }

    uint16_t row_time(const Row& row, int i) {
        return uint16_t((row.raw[6 + i * 2] << 8) | row.raw[7 + i * 2]);
    }

    void set_row_entry(Row& row, int i, uint8_t who, uint16_t time, const std::string& guest) {
        row.raw[i] = who;
        row.raw[6 + i * 2] = uint8_t(time >> 8);
        row.raw[7 + i * 2] = uint8_t(time);
        row.guest[i] = guest;
    }

    void move_row_entry(Row& row, int from, int to) {
        set_row_entry(row, to, row.raw[from], row_time(row, from), row.guest[from]);
    }

    bool is_guest(uint8_t who) {
        return who >= first_guest && who < first_guest + guest_count;
    }

    Row read_row(uint8_t* rdram, uint32_t addr) {
        Row row;
        for (uint32_t i = 0; i < row_size; i++) {
            row.raw[i] = MEM_BU(0, (int32_t)(addr + i));
        }
        for (int i = 0; i < 5; i++) {
            if (is_guest(row.raw[i])) {
                row.guest[i] = read_name(rdram, guest_names + (row.raw[i] - first_guest) * name_size);
            }
        }
        return row;
    }

    // The guest name entry holding name, preferring index (its entry when the row was read). If the game reused
    // that entry, finds the name elsewhere or takes a free entry; entry 0 (never handed out, blank) when the table
    // is full, as the game does when it reuses a name.
    int guest_entry(uint8_t* rdram, int index, const std::string& name) {
        auto entry_name = [&](int i) { return read_name(rdram, guest_names + i * name_size); };
        if (entry_name(index) == name) {
            return index;
        }
        if (name.empty()) {
            return 0;
        }
        for (int i = 0; i < guest_count; i++) {
            if (entry_name(i) == name) {
                return i;
            }
        }
        for (int i = 1; i < guest_count; i++) {
            if (MEM_BU(0, (int32_t)(guest_names + i * name_size)) == 0) {
                write_name(rdram, guest_names + i * name_size, name);
                MEM_H(0, (int32_t)(name_ages + i * 2)) = 0;
                return i;
            }
        }
        return 0;
    }

    void write_row(uint8_t* rdram, uint32_t addr, const Row& row) {
        Row out = row;
        for (int i = 0; i < 5; i++) {
            if (is_guest(out.raw[i])) {
                out.raw[i] = uint8_t(first_guest + guest_entry(rdram, out.raw[i] - first_guest, out.guest[i]));
            }
        }
        for (uint32_t i = 0; i < row_size; i++) {
            MEM_B(0, (int32_t)(addr + i)) = (int8_t)out.raw[i];
        }
    }

    // Where time t goes in the row (func_80094B2C): before the first empty or slower-or-equal entry; -1 if it is
    // slower than all five, or empty.
    int insert_position(const Row& row, uint16_t t) {
        if (t == 0) {
            return -1;
        }
        for (int i = 0; i < 5; i++) {
            uint16_t e = row_time(row, i);
            if (e == 0 || e >= t) {
                return i;
            }
        }
        return -1;
    }

    // Builds a 2049 row as func_80094BFC builds the game's: drop the profile entries, then merge the best times of
    // every profile in use on the paks.
    void merge_profiles(uint8_t* rdram, Row& row, int course, int page) {
        for (int pos = 0; pos < 5; pos++) {
            while (row.raw[pos] < first_guest) {
                for (int s = pos; s < 4; s++) {
                    move_row_entry(row, s + 1, s);
                }
                set_row_entry(row, 4, empty, 0, "");
            }
        }
        for (int p = 0; p < profiles; p++) {
            if ((MEM_BU(0, (int32_t)(record(p) + rec_flags)) & 1) == 0) {
                continue;
            }
            bind(rdram, p);
            for (int i = 0; i < 5; i++) {
                uint16_t t = MEM_HU(0, (int32_t)(times_in_block(p, course, page) + i * 2));
                int pos = insert_position(row, t);
                if (pos < 0) {
                    continue;
                }
                for (int s = 4; s > pos; s--) {
                    move_row_entry(row, s - 1, s);
                }
                set_row_entry(row, pos, uint8_t(p), t, "");
            }
        }
    }

    void swap_in(uint8_t* rdram, int k) {
        for (int backward = 0; backward < 2; backward++) {
            for (int page = 0; page < 2; page++) {
                uint32_t addr = row_address(page, backward);
                host_rows[backward][page] = read_row(rdram, addr);
                Row row = session_rows[course_of(k, backward)][page];
                merge_profiles(rdram, row, course_of(k, backward), page);
                write_row(rdram, addr, row);
            }
        }
        swapped = k;
    }

    void swap_out(uint8_t* rdram) {
        for (int backward = 0; backward < 2; backward++) {
            for (int page = 0; page < 2; page++) {
                uint32_t addr = row_address(page, backward);
                session_rows[course_of(swapped, backward)][page] = read_row(rdram, addr);
                write_row(rdram, addr, host_rows[backward][page]);
            }
        }
        swapped = 0;
    }

    float seed_2049(int k, int backward) {
        if (k == mod_track) {
            // The fastest of the track's best_times (track.toml), or 90 s.
            rush2::track_mod::Settings settings;
            float best = 0.0f;
            if (rush2::track_mod::race_settings(settings)) {
                for (float t : settings.best_times) if (t > 1.0f && (best == 0.0f || t < best)) best = t;
            }
            return best > 1.0f ? std::min(best, 479.0f) : 90.0f;
        }
        if (is_cs(k)) {
            float s = rush2::calspeed::record_seed(k - cs_track);
            return s > 1.0f ? std::min(s, 479.0f) : 90.0f;
        }
        // The table is indexed by Rush 2049's track id: 0-5 race tracks, 18 the obstacle course.
        int id = k == obstacle_track ? rush2::track2049::obstacle - 1 : k - 1;
        if (k > track_count && k != obstacle_track) {
            // SF Rush's own (0x800D157C), one per track.
            float s = rush2::track1::record_seed(k - track_count, backward != 0);
            return s > 1.0f ? s : 90.0f;
        }
        auto rom = rush2::wings::get_rom();
        auto boot = rom != nullptr ? rom->segment(rush2::rom2049::Segment::Boot) : nullptr;
        uint32_t at = seeds_2049_vram - rush2::rom2049::boot_vram + (uint32_t)(id + 19 * backward) * 4;
        if (boot != nullptr && boot->size() >= at + 4) {
            const std::vector<uint8_t>& b = *boot;
            uint32_t bits = (b[at] << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3];
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            if (f > 1.0f && f < 480.0f) {
                return f;
            }
        }
        return k == obstacle_track ? obstacle_seed : seeds_2049[backward][k - 1];
    }

    float host_seed(uint8_t* rdram, int backward) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(seeds + (host_slot + 12 * backward) * 4));
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // The records screen (menu overlay: func_803B0A90 per frame, func_803C430C draws the list). Its index
    // 0x803CB6FC is a track + 12 * backward (24 the circuit), its page 0x803CB6F8 0 average lap, 1 best lap, 2 stats.
    // While an added course is shown, the index stays on HAWAII's (2 or 14) and screen_k says which course it is, so
    // the screen and its logo widget run as they do for HAWAII and only the data they read is moved.
    constexpr uint32_t screen_index = 0x803CB6FC;
    constexpr uint32_t screen_page = 0x803CB6F8;
    constexpr uint32_t pipe_open = 0x800E7D50;    // The game's own unlock bytes the screen tests for PIPE (index 9)
    constexpr uint32_t midway_open = 0x800E7D19;  // and MIDWAY (10).
    constexpr uint32_t all_stats = 0x803D0298;    // [24] stats of every profile merged (func_803B1B78)
    constexpr uint32_t all_best_start = 0x803CACD0; // f32: func_803B1B78's starting best time
    // The logo containers the screen shows for index 2/14 (HAWAII's) and for 11 (STUNT1's).
    constexpr int logo_assets[2] = { 4 + host_slot, 4 + stunt_host };
    constexpr int logo_own[2] = { host_slot, stunt_host };
    int screen_k = 0;       // The added course shown, or 0.
    int render_k = 0;       // Its rows are swapped in while func_803C430C runs (a stunt arena has none).
    int shown_logo[2] = { -1, -1 }; // Track select id of the logo written into each of logo_assets (logo_own: its own).

    // The stats page's 16 icons (widgets of func_803C5514, slot n = widget +0x2C & 0xF): KEYICON for keys 0-11 and
    // DEWICON for Dew cans 12-15 (asset 2; 16 x 32 frames, left not found, right found), lit by bit n of the track's
    // mask. An SF Rush course shows its keys (bit n = key n + 1) and no cans; a 2049 race track its 16 coins, silver
    // bits 0-7 and gold bits 8-15: KEYICON holds the silver coin and DEWICON the gold one meanwhile, and slots 8-11
    // draw DEWICON. Other added courses (the obstacle course) show none.
    constexpr int icon_asset = 2;
    bool coin_icons = false;        // The coin pictures are in KEYICON / DEWICON.
    uint16_t icon_bits = 0;         // The shown course's mask, worked out by the list each frame.
    uint32_t icon_names = 0;        // RDRAM: "KEYICON", "DEWICON" (16 bytes each), for the widgets' texture names.

    int menu_id_of(int k) {
        if (is_stunt(k)) return rush2::track2049::stunt_menu_id + (k - stunt_track);
        if (k == obstacle_track) return rush2::track2049::obstacle_menu_id;
        if (is_cs(k)) return rush2::calspeed::first_menu_id + (k - cs_track);
        if (k > track_count) return rush2::track1::first_menu_id + (k - track_count - 1);
        return rush2::track2049::first_menu_id + (k - 1);
    }

    // Whether the screen's profile has track select entry t open (the unlock system's purchases are a profile's,
    // not the joined players'): the profile it views (0x803D026C >= 0: pak 0x803D0274, record 0x803D027C), or on
    // TOTALS any profile on the paks.
    constexpr uint32_t screen_profile = 0x803D026C;  // s16: < 0 TOTALS
    constexpr uint32_t screen_pak = 0x803D0274;      // s16
    constexpr uint32_t screen_record = 0x803D027C;   // s16

    bool screen_open(uint8_t* rdram, int t, bool game_unlocked = true) {
        if ((int16_t)MEM_H(0, (int32_t)screen_profile) >= 0) {
            int p = (int16_t)MEM_H(0, (int32_t)screen_pak) * records_per_pak + (int16_t)MEM_H(0, (int32_t)screen_record);
            return p >= 0 && p < profiles &&
                   rush2::unlocks::track_open_for(read_name(rdram, record(p) + rec_name), t, game_unlocked);
        }
        for (int p = 0; p < profiles; p++) {
            if ((MEM_BU(0, (int32_t)(record(p) + rec_flags)) & 1) != 0 &&
                rush2::unlocks::track_open_for(read_name(rdram, record(p) + rec_name), t, game_unlocked)) {
                return true;
            }
        }
        return rush2::unlocks::track_open_for("", t, game_unlocked);   // no profile: the guest's purchases
    }

    // The screen's index while it shows added course k.
    int screen_index_of(int k, int backward) {
        return is_stunt(k) ? stunt_host : host_slot + 12 * backward;
    }

    // Whether the screen is on its added course (its index is the one the course is shown on).
    bool screen_on_course(uint8_t* rdram) {
        int index = MEM_W(0, (int32_t)screen_index);
        return screen_k != 0 && (index == screen_index_of(screen_k, 0) || index == screen_index_of(screen_k, 1));
    }

    // The added courses to list, in track select order (the stunt arenas are listed on the stats page only).
    std::vector<int> screen_courses(uint8_t* rdram) {
        std::vector<int> ks;
        for (int k = 1; k <= obstacle_track - 1; k++) {
            bool on = k <= track_count ? rush2::track2049::available() : rush2::track1::available();
            if (on && screen_open(rdram, menu_id_of(k))) ks.push_back(k);
        }
        for (int k = stunt_track; k < stunt_track + stunt_count; k++) {
            if (rush2::track2049::available() && screen_open(rdram, menu_id_of(k))) ks.push_back(k);
        }
        if (rush2::track2049::available() && screen_open(rdram, menu_id_of(obstacle_track))) {
            ks.push_back(obstacle_track);
        }
        for (int k = cs_track; k < cs_track + cs_count; k++) {
            if (rush2::calspeed::available() && screen_open(rdram, menu_id_of(k))) ks.push_back(k);
        }
        return ks;
    }

    struct ScreenEntry {
        int index, k;
    };

    // What left and right step through on a page: Rush 2's entries as func_803B0A90 picks them (PIPE and MIDWAY
    // when open, per screen_open, STUNT1 and the circuit on the stats page only), the added courses forward after Rush 2's forward
    // tracks and backward after its backward ones (the obstacle course has no backward).
    std::vector<ScreenEntry> screen_entries(uint8_t* rdram, const std::vector<int>& ks) {
        int page = MEM_W(0, (int32_t)screen_page);
        auto shown = [&](int index) {
            int m = index % 12;
            if (m < 9) return true;
            // PIPE and MIDWAY: the game's own unlock, or with the unlock system on, the profile's purchase.
            if (m == 9) return screen_open(rdram, m, MEM_BU(0, (int32_t)pipe_open) != 0);
            if (m == 10) return screen_open(rdram, m, MEM_BU(0, (int32_t)midway_open) != 0);
            return page == 2 && index == 11;
        };
        std::vector<ScreenEntry> e;
        for (int backward = 0; backward < 2; backward++) {
            for (int index = backward * 12; index < backward * 12 + 12; index++) {
                if (shown(index)) e.push_back({ index, 0 });
            }
            // The stunt arenas, like STUNT1, are forward and on the stats page only.
            for (int k : ks) {
                if (is_stunt(k) ? page == 2 && !backward : !backward || (k != obstacle_track && !is_cs(k))) {
                    e.push_back({ screen_index_of(k, backward), k });
                }
            }
        }
        if (page == 2) e.push_back({ 24, 0 });
        return e;
    }

    // func_803B1B78's merge of every profile's stats for one index (the ALL stats page), for course k: the
    // no-profile stats, then per profile in use the average lap weighted by laps, the counters summed, +0x02 the
    // largest, and +0x01 from the profile with the fastest first average-lap time.
    void screen_all_stats(uint8_t* rdram, int k, int backward) {
        auto seconds = [](uint16_t t) { return float(t >> 13) * 60.0f + float((t >> 7) & 0x3F) + float(t & 0x7F) / 100.0f; };
        int course = course_of(k, backward);
        uint32_t agg = all_stats_block;
        for (uint32_t i = 0; i < stats_size; i++) {
            MEM_B(0, (int32_t)(agg + i)) = MEM_B(0, (int32_t)(no_profile_block + course * stats_size + i));
        }
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)all_best_start);
        float best;
        std::memcpy(&best, &bits, sizeof(best));
        for (int p = 0; p < profiles; p++) {
            if ((MEM_BU(0, (int32_t)(record(p) + rec_flags)) & 1) == 0) {
                continue;
            }
            bind(rdram, p);
            uint32_t s = block(p) + course * stats_size;
            float total = seconds(MEM_HU(4, (int32_t)agg)) * float(MEM_HU(6, (int32_t)agg)) +
                          seconds(MEM_HU(4, (int32_t)s)) * float(MEM_HU(6, (int32_t)s));
            uint16_t laps = uint16_t(MEM_HU(6, (int32_t)agg) + MEM_HU(6, (int32_t)s));
            MEM_H(6, (int32_t)agg) = (int16_t)laps;
            if (laps > 0) {
                float avg = total / float(laps);
                uint32_t whole = (uint32_t)avg, min = whole / 60, sec = whole % 60, hund = (uint32_t)(avg * 100.0f) % 100;
                if (min >= 8) min = 7, sec = 59, hund = 99;   // func_80094A40's clamp
                MEM_H(4, (int32_t)agg) = (int16_t)((hund & 0x7F) | ((sec & 0x3F) << 7) | (min << 13));
            }
            for (int off = 8; off <= 0x1E; off += 2) {
                MEM_H(off, (int32_t)agg) = (int16_t)(MEM_HU(off, (int32_t)agg) + MEM_HU(off, (int32_t)s));
            }
            if (MEM_BU(2, (int32_t)agg) < MEM_BU(2, (int32_t)s)) {
                MEM_B(2, (int32_t)agg) = MEM_B(2, (int32_t)s);
            }
            float first = seconds(MEM_HU(0, (int32_t)times_in_block(p, course, 0)));
            if (first < best) {
                best = first;
                MEM_B(1, (int32_t)agg) = MEM_B(1, (int32_t)s);
            }
        }
    }

    // Whether added course k's stats page shows Rush 2049 coins.
    bool coin_course(int k) {
        return (k >= 1 && k <= track_count) || is_stunt(k);
    }

    // Course k's keys or coins found by the screen's profile, or on TOTALS by every profile on the paks and the
    // players without one (as func_803B1B78 ORs Rush 2's key masks).
    uint16_t screen_found(uint8_t* rdram, int k) {
        if (k < 1 || k == obstacle_track || k == mod_track || is_cs(k)) return 0;
        std::set<std::string> names;
        bool totals = (int16_t)MEM_H(0, (int32_t)screen_profile) < 0;
        if (!totals) {
            int p = (int16_t)MEM_H(0, (int32_t)screen_pak) * records_per_pak + (int16_t)MEM_H(0, (int32_t)screen_record);
            if (p < 0 || p >= profiles) return 0;
            names.insert(read_name(rdram, record(p) + rec_name));
        }
        else {
            for (int p = 0; p < profiles; p++) {
                if ((MEM_BU(0, (int32_t)(record(p) + rec_flags)) & 1) != 0) names.insert(read_name(rdram, record(p) + rec_name));
            }
            names.insert("");
        }
        uint16_t bits = 0;
        for (const rush2::collectibles::Progress& pr : rush2::collectibles::progress()) {
            if (!names.contains(pr.name)) continue;
            bits |= is_stunt(k) ? pr.stunt2049[size_t(k - stunt_track)]
                  : k <= track_count ? pr.rush2049[size_t(k - 1)] : pr.sfrush[size_t(k - track_count - 1)];
        }
        return bits;
    }

    // Puts the shown entry's logo into the logo container its index shows, HAWAII's or for a stunt arena STUNT1's
    // (each container's own for anything else).
    void screen_logo(uint8_t* rdram, recomp_context* ctx, int k) {
        for (int i = 0; i < 2; i++) {
            int want = k != 0 && is_stunt(k) == (i == 1) ? menu_id_of(k) : logo_own[i];
            if (want != shown_logo[i] && rush2::track2049::write_menu_logo(rdram, ctx, logo_assets[i], want)) {
                shown_logo[i] = want;
            }
        }
        // The coin pictures, built once from the 2049 ROM.
        static std::vector<uint8_t> coin_texels[2], coin_palette[2];
        bool coins = coin_course(k);
        if (coins && coin_texels[0].empty()) {
            auto rom = rush2::wings::get_rom();
            for (int gold = 0; gold < 2 && rom != nullptr; gold++) {
                if (!rush2::track2049::coin_icon(*rom, gold != 0, coin_texels[gold], coin_palette[gold])) {
                    coin_texels[0].clear();
                    break;
                }
            }
        }
        if (coins && coin_texels[0].empty()) {
            coins = false;
        }
        if (coins != coin_icons) {
            static const std::vector<uint8_t> none;
            rush2::track2049::write_menu_texture(rdram, ctx, icon_asset, "KEYICON", coins ? coin_texels[0] : none,
                                                 coins ? coin_palette[0] : none);
            rush2::track2049::write_menu_texture(rdram, ctx, icon_asset, "DEWICON", coins ? coin_texels[1] : none,
                                                 coins ? coin_palette[1] : none);
            coin_icons = coins;
        }
    }
}

extern "C" void menu_play_sound_80064908(uint8_t* rdram, recomp_context* ctx);
extern "C" void records_screen_spawn_random_particles_803B0650(uint8_t* rdram, recomp_context* ctx);

// func_803B0A90 at 0x803B0B64, the screen opening, after it loaded the logo containers (assets 4-16). It starts on
// index 0. A race on an added track leaves its host's container (HAWAII's, or STUNT1's for a stunt arena) holding that
// track's logo, so their own go back. Stunt stats not yet saved (a session quit before its end) are saved here.
extern "C" void rush2_records_screen_open(uint8_t* rdram, recomp_context* ctx) {
    screen_k = 0;
    shown_logo[0] = shown_logo[1] = -1;
    screen_logo(rdram, ctx, 0);
    std::lock_guard lock{ records_mutex };
    save_changes(rdram);
}

// func_803B0A90 at 0x803B0AF0, the screen closing: the hosts' logos go back for whatever loads them next.
extern "C" void rush2_records_screen_close(uint8_t* rdram, recomp_context* ctx) {
    screen_k = 0;
    if (shown_logo[0] != -1 || shown_logo[1] != -1 || coin_icons) screen_logo(rdram, ctx, 0);
    shown_logo[0] = shown_logo[1] = -1;
}

// func_803B0A90 at 0x803B0BC8: $v0 = the buttons pressed, about to be tested for left/right (0x200 left, 0x100
// right) to step the index. With added courses to list, this steps through screen_entries instead and clears the
// two bits so the game's own step is skipped, then plays the step's sound and particles as the game does.
extern "C" void rush2_records_screen_step(uint8_t* rdram, recomp_context* ctx) {
    uint32_t buttons = (uint32_t)ctx->r2;
    if ((buttons & 0x300) == 0) {
        return;
    }
    std::vector<int> ks = screen_courses(rdram);
    if (ks.empty() && !rush2::unlocks::enabled()) {
        screen_k = 0;
        return;
    }
    std::vector<ScreenEntry> e = screen_entries(rdram, ks);
    int index = MEM_W(0, (int32_t)screen_index);
    int at = -1;
    for (size_t i = 0; i < e.size(); i++) {
        if (e[i].index == index && e[i].k == screen_k) at = int(i);
    }
    if (at < 0) {
        screen_k = 0;
        return;
    }
    int n = int(e.size());
    at = (at + ((buttons & 0x200) ? -1 : 1) + n) % n;
    MEM_W(0, (int32_t)screen_index) = e[at].index;
    screen_k = e[at].k;
    ctx->r2 = buttons & ~0x300u;
    recomp_context call = *ctx;
    call.r4 = 0x49;
    menu_play_sound_80064908(rdram, &call);
    call = *ctx;
    records_screen_spawn_random_particles_803B0650(rdram, &call);
    screen_logo(rdram, ctx, screen_k);
}

// Start of func_803C430C (the records list): swaps the shown course's high-score rows in for HAWAII's (the ALL
// pages read them), as the race screens do.
extern "C" void rush2_records_screen_render_enter(uint8_t* rdram, recomp_context* ctx) {
    // Leaving the stats page moves the game's index off STUNT1's (11 to 12), and so off a stunt arena.
    if (is_stunt(screen_k) && !screen_on_course(rdram)) {
        screen_k = 0;
        screen_logo(rdram, ctx, 0);
    }
    icon_bits = screen_k != 0 ? screen_found(rdram, screen_k) : 0;
    std::lock_guard lock{ records_mutex };
    render_k = 0;
    if (!screen_on_course(rdram) || swapped != 0) {
        return;
    }
    ensure_blocks(rdram);
    if (!is_stunt(screen_k)) {
        swap_in(rdram, screen_k);
    }
    render_k = screen_k;
}

extern "C" void hud_widget_set_texture_by_name_80080D40(uint8_t* rdram, recomp_context* ctx);
extern "C" void hud_widget_init_type_80060DA8(uint8_t* rdram, recomp_context* ctx);
extern "C" void hud_widget_setup_icon_80080DBC(uint8_t* rdram, recomp_context* ctx);

// Start of func_803C5514, a stats page icon ($a0 = the widget, returns 1). Gives slots 8-11 DEWICON while the coins
// are shown (KEYICON otherwise), and for an added course does the function's work with the course's keys or coins
// instead of HAWAII's: hidden off the stats page, else lit by its bit. Returns 1 when it did (the function is skipped).
extern "C" int rush2_records_screen_icon(uint8_t* rdram, recomp_context* ctx) {
    uint32_t w = (uint32_t)ctx->r4;
    int n = MEM_W(0x2C, (int32_t)w) & 0xF;
    if (icon_names == 0) {
        icon_names = (uint32_t)((uint8_t*)recomp::alloc(rdram, 32) - rdram) + 0x80000000;
        write_name(rdram, icon_names, "KEYICON");
        write_name(rdram, icon_names + 16, "DEWICON");
    }
    if (n < 12) {
        uint32_t want = icon_names + (coin_icons && n >= 8 ? 16 : 0);
        uint32_t name = (uint32_t)MEM_W(0, (int32_t)w);
        if (name == 0 || read_name(rdram, name) != read_name(rdram, want)) {
            recomp_context call = *ctx;
            call.r4 = (gpr)(int32_t)w;
            call.r5 = (gpr)(int32_t)want;
            call.r6 = 1;
            hud_widget_set_texture_by_name_80080D40(rdram, &call);
        }
    }
    if (!screen_on_course(rdram)) {
        return 0;
    }
    int k = screen_k;
    bool shown = MEM_W(0, (int32_t)screen_page) == 2 &&
                 (coin_course(k) ? coin_icons
                                 : k > track_count && k < obstacle_track && n < rush2::collectibles::sfrush_keys[size_t(k - track_count - 1)]);
    recomp_context call = *ctx;
    call.r4 = (gpr)(int32_t)w;
    call.r5 = shown ? 0 : 1;
    hud_widget_init_type_80060DA8(rdram, &call);
    if (shown) {
        call = *ctx;
        call.r4 = (gpr)(int32_t)w;
        call.r5 = (icon_bits >> n) & 1;
        call.r6 = 0x10;
        call.r7 = 0x20;
        hud_widget_setup_icon_80080DBC(rdram, &call);
    }
    ctx->r2 = 1;
    return 1;
}

// The jr $ra of func_803C430C (0x803C5460).
extern "C" void rush2_records_screen_render_exit(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    if (render_k != 0) {
        if (swapped != 0) {
            swap_out(rdram);
        }
        render_k = 0;
    }
}

// func_803C430C at 0x803C45BC (pages 0 and 1): $a3 = the five times to list (a high-score row's, or one profile's
// best-times list in the pak image, which moves to the course's), $f6 = the seed time of the index (0x8001CF3C),
// from which empty rows' made-up times start: moved to the course's.
extern "C" void rush2_records_screen_times(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    if (render_k == 0 || is_stunt(render_k)) {
        return;
    }
    int backward = MEM_W(0, (int32_t)screen_index) >= 12 ? 1 : 0;
    ctx->r7 = (gpr)(int32_t)redirect_times_to(rdram, (uint32_t)ctx->r7, render_k);
    ctx->f6.fl += seed_2049(render_k, backward) - host_seed(rdram, backward);
}

// func_803C430C at 0x803C49CC (the stats page): 0x68($sp) = the stats to show, ALL's merged entry (0x803D0298 +
// index * 0x20) or a profile's slot in the pak image. Moved to the course's. Index 11 (STUNT1, and so a stunt arena)
// has its own layout from there to 0x803C4D58: the stunt counters +0x16-0x1C, then the bests +0x02 and +0x1E.
extern "C" void rush2_records_screen_stats(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    if (render_k == 0) {
        return;
    }
    int index = MEM_W(0, (int32_t)screen_index);
    uint32_t addr = (uint32_t)MEM_W(0x68, (int32_t)ctx->r29);
    if (addr == all_stats + uint32_t(index) * stats_size) {
        screen_all_stats(rdram, render_k, index >= 12 ? 1 : 0);
        addr = all_stats_block;
    }
    else {
        addr = redirect_stats_to(rdram, addr, render_k);
    }
    MEM_W(0x68, (int32_t)ctx->r29) = (int32_t)addr;
}

// Stats writers, where $t3 = the stats slot (record + 0x1F0 + slot * 0x20, or the no-profile table entry) that
// the code that follows reads and writes:
// - func_80065360 at 0x80065428, func_8006544C at 0x80065514 (stunt scoring), func_800A9164 at 0x800A922C
//   (both paths join there, after the profile path's dirty marking).
// - func_800A9250 at 0x800A93B4 (race count, places): same.
// - func_800A7804 (average lap): at 0x800A78E4, the profile path's jal 0x8005F338 (before the record byte +1 is
//   written), and at 0x800A78F8, where the no-profile path joins.
extern "C" void rush2_track49_records_stats(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    ctx->r11 = (gpr)(int32_t)redirect_stats(rdram, (uint32_t)ctx->r11);
}

// func_800A952C at 0x800A99C8: $t3 = the player's best-times list for the page (record + 0x38 + page * 0xDC +
// slot * 0xA), about to be passed to func_80094B2C ($a3) and func_80094A98 ($s3).
extern "C" void rush2_track49_records_times(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    ctx->r11 = (gpr)(int32_t)redirect_times(rdram, (uint32_t)ctx->r11);
}

// Start of func_800A952C (post-race records) and func_800606A8 (high-score screen). The first one in swaps the 2049
// track's high-score rows in.
extern "C" void rush2_track49_records_enter(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    if (depth++ > 0) {
        return;
    }
    int k = hosted(rdram);
    if (k != 0) {
        ensure_blocks(rdram);
        sync_mod(rdram);
        swap_in(rdram, k);
    }
}

// The jr $ra of func_800A952C (0x800AA964) and func_800606A8 (0x80060DA0). The last one out swaps HAWAII's rows
// back and saves changed 2049 records.
extern "C" void rush2_track49_records_exit(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    if (depth == 0 || --depth > 0) {
        return;
    }
    if (swapped != 0) {
        swap_out(rdram);
    }
    save_changes(rdram);
}

// Start of func_800B3110 (records screen: clear a profile's records): $a0 = the profile index. Clears that profile's
// 2049 records too, and, as the game clears its keys there, its SF Rush keys and 2049 coins. Deleting a profile does
// the same (src/pak.cpp).
extern "C" void rush2_track49_records_clear(uint8_t* rdram, recomp_context* ctx) {
    rush2::track2049::clear_profile_records(rdram, (int32_t)ctx->r4);
    rush2::collectibles::clear_profile(rdram, (int32_t)ctx->r4);
}

void rush2::track2049::clear_profile_records(uint8_t* rdram, int p) {
    std::lock_guard lock{ records_mutex };
    if (p < 0 || p >= profiles) {
        return;
    }
    ensure_blocks(rdram);
    std::string name = read_name(rdram, record(p) + rec_name);
    for (int q = 0; q < profiles; q++) {
        if (bound[q] && bound_name[q] == name) {
            write_block(rdram, q, Profile{});
            bound_copy[q] = Profile{};
        }
    }
    bool had = store.erase(name) != 0;
    had = mod_store.erase(name) != 0 || had;
    if (had) {
        save_store();
    }
}

// func_800606A8 at 0x800608A4: $f20 = the time the made-up entries of empty rows start from: the seed time of the
// track (0x8001CF3C[track + 12 * backward]), plus 1-3 s on page 0. Moves it to the 2049 track's seed.
extern "C" void rush2_track49_records_seed(uint8_t* rdram, recomp_context* ctx) {
    int k = hosted(rdram);
    if (k == 0) {
        return;
    }
    int backward = MEM_B(0, (int32_t)backward_flag) != 0 ? 1 : 0;
    uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(seeds + (host_slot + 12 * backward) * 4));
    float host_seed;
    std::memcpy(&host_seed, &bits, sizeof(host_seed));
    ctx->f20.fl += seed_2049(k, backward) - host_seed;
}
