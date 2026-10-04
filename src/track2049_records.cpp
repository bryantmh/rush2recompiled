// Records for the Rush 2049 tracks (docs/rush2049_research/records.md).
//
// A 2049 track is raced in the host slot (HAWAII, src/track2049.cpp), and Rush 2 indexes its records by track, so
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
//   and saved to track2049_records.json in the app folder. The no-profile stats go to a session-only block, as
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
// Menu screens (the records screen, profile loading) run outside those windows and only ever see HAWAII's records;
// 2049 records aren't listed on the records screen, and 2049 results don't count toward unlocks. Clearing a
// profile's records there, or deleting the profile, clears its 2049 records as well.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include "json/json.hpp"

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"
#include "rush2_hooks.h"
#include "track2049.h"
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

    // The 2049 tracks forward and backward: course = (track - 1) + 6 * backward.
    constexpr int courses = track_count * 2;

    // A side block in RDRAM per player-record position: the stats of each course, then its two pages of times.
    constexpr uint32_t block_times = courses * stats_size;
    constexpr uint32_t block_size = block_times + courses * 2 * list_size;

    // Rush 2049's seed times for its race tracks (boot segment 0x8002E870, f32, index track + 19 * backward), used
    // when the ROM can't be read.
    constexpr uint32_t seeds_2049_rom = 0x1000 + (0x8002E870 - 0x80000400);
    constexpr float seeds_2049[2][track_count] = { { 47, 81, 65, 97, 78, 117 }, { 48, 79, 76, 104, 86, 119 } };

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
    std::map<std::string, Profile> store;   // By profile name, as in track2049_records.json.
    bool store_loaded = false;
    bool store_dirty = false;

    uint32_t blocks = 0;        // RDRAM: one block per position, then the no-profile stats.
    uint32_t no_profile_block = 0;
    std::string bound_name[profiles];
    bool bound[profiles] = {};
    Profile bound_copy[profiles];  // The block as last loaded or saved.
    bool touched = false;          // A side block was handed to the game since the last save.

    Row session_rows[courses][2]; // The 2049 high-score rows while swapped out (guests; profiles are re-merged).
    Row host_rows[2][2];          // HAWAII's rows while the 2049 ones are swapped in, [backward][page].
    int depth = 0;
    int swapped = 0;              // The 2049 track whose rows are swapped in, or 0.

    int course_of(int k, int backward) {
        return (k - 1) + track_count * backward;
    }

    // The 2049 track hosted by the current race (1-6), or 0.
    int hosted(uint8_t* rdram) {
        int k = rush2::track2049::race_track();
        if (k < 1 || k > track_count || MEM_B(0, (int32_t)track_id) != host_slot) {
            return 0;
        }
        return k;
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

    // Side file

    std::filesystem::path records_path() {
        return recompui::file::get_app_folder_path() / "track2049_records.json";
    }

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

    void load_store() {
        if (store_loaded) {
            return;
        }
        store_loaded = true;
        std::ifstream f(records_path());
        if (!f) {
            return;
        }
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.contains("profiles") || !j["profiles"].is_array()) {
            printf("[2049] Couldn't read %s\n", records_path().string().c_str());
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
                if (!cj.is_object() || !cj.contains("track") || !cj["track"].is_number_integer()) {
                    continue;
                }
                int k = cj["track"].get<int>();
                int backward = cj.value("backward", false) ? 1 : 0;
                if (k < 1 || k > track_count) {
                    continue;
                }
                Course& c = profile[course_of(k, backward)];
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
        }
    }

    void save_store() {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [name, profile] : store) {
            nlohmann::json course_list = nlohmann::json::array();
            for (int course = 0; course < courses; course++) {
                const Course& c = profile[course];
                if (c.zero()) {
                    continue;
                }
                nlohmann::json pages[2] = { nlohmann::json::array(), nlohmann::json::array() };
                for (int page = 0; page < 2; page++) {
                    for (int i = 0; i < 5; i++) {
                        pages[page].push_back(c.times[page][i] != 0 ? nlohmann::json(format_time(c.times[page][i]))
                                                                     : nlohmann::json(nullptr));
                    }
                }
                course_list.push_back({ { "track", course % track_count + 1 },
                                        { "backward", course >= track_count },
                                        { "race", pages[0] },
                                        { "lap", pages[1] },
                                        { "stats", to_hex(c.stats, sizeof(c.stats)) } });
            }
            if (!course_list.empty()) {
                list.push_back({ { "name", printable(name) },
                                 { "key", to_hex((const uint8_t*)name.data(), name.size()) },
                                 { "courses", course_list } });
            }
        }
        std::filesystem::path path = records_path();
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::trunc);
            if (!f) {
                printf("[2049] Couldn't write %s\n", temp.string().c_str());
                return;
            }
            f << nlohmann::json{ { "version", 1 }, { "profiles", list } }.dump(2) << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            printf("[2049] Couldn't replace %s: %s\n", path.string().c_str(), ec.message().c_str());
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
        uint32_t size = profiles * block_size + courses * stats_size;
        blocks = (uint32_t)((uint8_t*)recomp::alloc(rdram, size) - rdram) + 0x80000000;
        no_profile_block = blocks + profiles * block_size;
        for (uint32_t i = 0; i < size; i++) {
            MEM_B(0, (int32_t)(blocks + i)) = 0;
        }
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

    // Address redirection. Each returns addr unchanged unless it is one of HAWAII's slots and a 2049 track is hosted.

    // A stats slot: record + 0x1F0 + slot * 0x20 in the pak image, or an entry of the no-profile table.
    uint32_t redirect_stats(uint8_t* rdram, uint32_t addr) {
        int k = hosted(rdram);
        if (k == 0) {
            return addr;
        }
        ensure_blocks(rdram);
        for (int backward = 0; backward < 2; backward++) {
            if (addr == no_profile_stats + host_no_profile_slot[backward] * stats_size) {
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
            if (field == rec_stats + host_record_slot[backward] * stats_size) {
                bind(rdram, p);
                touched = true;
                return block(p) + course_of(k, backward) * stats_size;
            }
        }
        return addr;
    }

    // A best-times list: record + 0x38 + page * 0xDC + slot * 0xA in the pak image.
    uint32_t redirect_times(uint8_t* rdram, uint32_t addr) {
        int k = hosted(rdram);
        uint32_t off = addr - pak_records;
        if (k == 0 || addr < pak_records || off >= 4 * pak_stride ||
            off % pak_stride >= records_per_pak * record_size) {
            return addr;
        }
        ensure_blocks(rdram);
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
        auto rom = rush2::wings::get_rom();
        uint32_t at = seeds_2049_rom + (uint32_t)((k - 1) + 19 * backward) * 4;
        if (rom != nullptr && rom->size() >= at + 4) {
            uint32_t bits = ((*rom)[at] << 24) | ((*rom)[at + 1] << 16) | ((*rom)[at + 2] << 8) | (*rom)[at + 3];
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            if (f > 1.0f && f < 480.0f) {
                return f;
            }
        }
        return seeds_2049[backward][k - 1];
    }
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

// Start of func_800B3110 (records screen: clear a profile's records) and func_800B306C (delete a profile): $a0 = the
// profile index. Clears that profile's 2049 records too.
extern "C" void rush2_track49_records_clear(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ records_mutex };
    int p = (int32_t)ctx->r4;
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
    if (store.erase(name) != 0) {
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
