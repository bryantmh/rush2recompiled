#ifndef __CAR_TYPES_H__
#define __CAR_TYPES_H__

#include <cstdint>

// The car types and where their per-type data lives in RDRAM (docs/rush2049_research/cars.md, Implementation).
//
// Rush 2's types are 0-21 and 22 is its "no car" marker; Rush 2049's cars are types 23-35 (src/rush2049/car2049.cpp)
// and California Speed's 36-56 (src/calspeed/calspeed_cars.cpp). Rush 2 keeps its per-type data in tables of 22
// entries, which us.toml points at wider copies (tools/rush2049/cartypes.py generates those patches from the same
// layout as here: change one, regenerate the other).
namespace rush2::car_types {
    constexpr int rush2_types = 22;
    constexpr int no_car = 22;
    constexpr int first_2049 = 23, count_2049 = 13;
    constexpr int first_calspeed = 36, count_calspeed = 21;
    constexpr int types = first_calspeed + count_calspeed; // 57
    // The car select's preview ids: player * types + type, two panels.
    constexpr int preview_ids = 2 * types;

    constexpr bool is_2049(int type) { return type >= first_2049 && type < first_2049 + count_2049; }
    constexpr bool is_calspeed(int type) { return type >= first_calspeed && type < types; }

    // Rush 2's per-type tables (0x800C06B4-0x800C0FDE): old base, bytes per entry, rows (1, or 3: the computer cars,
    // players 1 and 2). The copies of the per-player tables have player_rows rows (players 1-8 read row player + 1:
    // players 3 and 4 locally, src/players4.cpp, and 5 to 8 online, docs/online.md#eight-players), one 64 KB page from
    // 0x80200000 (cartypes.py layout).
    constexpr int player_rows = 9;
    struct Table {
        uint32_t old_base;
        int size;
        int rows;
    };
    constexpr Table tables[] = {
        { 0x800C06B4, 4, 1 }, // steer/yaw force base
        { 0x800C070C, 4, 1 }, // yaw damping base
        { 0x800C0764, 4, 1 }, // car name pointers
        { 0x800C07E8, 1, 1 }, // type -> physics descriptor index
        { 0x800C0800, 4, 1 }, // suspension preload
        { 0x800C0858, 4, 1 }, // mass
        { 0x800C08B0, 4, 1 }, // pitch/roll inertia
        { 0x800C090C, 4, 1 }, // yaw inertia
        { 0x800C0964, 4, 3 }, // setup weight value (DURABILITY)
        { 0x800C0A6C, 4, 3 }, // front wheel scale
        { 0x800C0B74, 4, 3 }, // rear wheel scale
        { 0x800C0C7C, 1, 3 }, // ENGINE (engine sound)
        { 0x800C0CD0, 1, 3 }, // engine torque map index (TORQUE)
        { 0x800C0D14, 1, 3 }, // drive flags
        { 0x800C0D68, 1, 3 }, // SUSPENSION
        { 0x800C0DAC, 1, 3 }, // TIRES (handling 0-10)
        { 0x800C0E48, 1, 3 }, // TIRE RIMS
        { 0x800C0E8C, 1, 3 }, // HORN
        { 0x800C0ED0, 1, 3 }, // MAIN COLOR
        { 0x800C0F14, 1, 3 }, // ACCENT COLOR
        { 0x800C0F58, 1, 3 }, // STRIPE
        { 0x800C0F9C, 1, 3 }, // STRIPE COLOR
    };
    constexpr uint32_t tables_base = 0x80200000;
    constexpr int copy_rows(const Table& t) { return t.rows == 3 ? player_rows : t.rows; }
    // The copy of the table at old_base (0 if it isn't one).
    constexpr uint32_t table(uint32_t old_base) {
        uint32_t at = tables_base;
        for (const Table& t : tables) {
            if (t.old_base == old_base) return at;
            at += (uint32_t(types * t.size * copy_rows(t)) + 7) & ~7u;
        }
        return 0;
    }
    constexpr uint32_t tables_end() {
        uint32_t at = tables_base;
        for (const Table& t : tables) at += (uint32_t(types * t.size * copy_rows(t)) + 7) & ~7u;
        return at;
    }
    // Entry `type` of row `row` (0 the computer cars, player + 1) of the copy of the table at old_base.
    constexpr uint32_t entry(uint32_t old_base, int row, int type) {
        for (const Table& t : tables) {
            if (t.old_base == old_base) return table(old_base) + uint32_t((row * types + type) * t.size);
        }
        return 0;
    }
    constexpr uint32_t steer_base = 0x800C06B4, yaw_damping = 0x800C070C, name_pointers = 0x800C0764,
                       desc_index = 0x800C07E8, preload = 0x800C0800, mass = 0x800C0858, inertia = 0x800C08B0,
                       yaw_inertia = 0x800C090C, weight = 0x800C0964, front_wheel_scale = 0x800C0A6C,
                       rear_wheel_scale = 0x800C0B74, engine = 0x800C0C7C, torque = 0x800C0CD0, drive = 0x800C0D14,
                       suspension = 0x800C0D68, tires = 0x800C0DAC, rims = 0x800C0E48, horn = 0x800C0E8C,
                       main_color = 0x800C0ED0, accent_color = 0x800C0F14, stripe = 0x800C0F58,
                       stripe_color = 0x800C0F9C;
    static_assert(table(steer_base) == tables_base);
    static_assert(tables_end() <= 0x80208000, "the type tables run into the preview id arrays");

    // Race per-car arrays the car select also indexes by preview id: 44 entries moved to preview_ids, in one 64 KB
    // window (one lui value, 0x8021) from 0x80208000 (cartypes.py id_layout). Only the first 8 entries (the race cars)
    // were Rush 2's; the car select's ids past them indexed other variables.
    struct IdArray {
        uint32_t old_base;
        uint32_t stride;
    };
    constexpr int old_ids = 44;
    constexpr IdArray id_arrays[] = {
        { 0x8010C480, 0x4E },  // part handles (func_80086700)
        { 0x8010F330, 0x1C },  // per-car callbacks
        { 0x80113F90, 0x134 }, // car state (body node index at +0)
        { 0x801174F8, 0x7C },  // preview state (+1 loaded)
        { 0x800D9D40, 0x2 },   // per-car s16 (texture bank)
        { 0x800D97E8, 0x18 },  // car slot loads (func_80087CE4): +0xC the palette record
        { 0x80119858, 0x1 },   // per-car byte (func_8008582C)
    };
    constexpr uint32_t id_window = 0x80208000;
    constexpr uint32_t id_array(uint32_t old_base) {
        uint32_t at = id_window;
        for (const IdArray& a : id_arrays) {
            if (a.old_base == old_base) return at;
            at += (uint32_t(preview_ids) * a.stride + 15) & ~15u;
        }
        return 0;
    }
    constexpr uint32_t id_window_end() {
        uint32_t at = id_window;
        for (const IdArray& a : id_arrays) at += (uint32_t(preview_ids) * a.stride + 15) & ~15u;
        return at;
    }
    static_assert(id_window_end() <= id_window + 0x10000, "the preview id arrays need one lui value");
    constexpr uint32_t part_handles = id_array(0x8010C480);  // + id * 0x4E: the 39 part model handles
    constexpr uint32_t car_states = id_array(0x80113F90);    // + car * 0x134: +0 the car's body scene node index
    constexpr uint32_t car_previews = id_array(0x801174F8);  // + id * 0x7C
    constexpr uint32_t car_banks = id_array(0x800D9D40);     // s16 per id: the car's texture bank
    // + id * 0x18: the car's palette record (name[16], first and last index, +0x14 the palette's address).
    constexpr uint32_t car_palette_records = id_array(0x800D97E8);
    constexpr uint32_t car_palettes = car_palette_records + 0x14;

    // The car select's per-player car list (car i of player p at + 2 * i + p; moved from 0x803CB368) and carousel
    // (0x18 bytes a car, 0x810 a player; moved from 0x803CB980, 0x210 a player): us.toml.
    constexpr uint32_t car_list = 0x80222000;
    constexpr uint32_t carousel = 0x80222100;
    constexpr uint32_t carousel_stride = 0x810;
    static_assert(types * 0x18 <= int(carousel_stride));

    // The extra types' other data, in the RDRAM the preview id arrays moved out of (0x80218000-0x80222000).
    constexpr int asset_slots = 0x100;                // asset indices the moved asset tables have room for (u8)
    constexpr uint32_t asset_offsets = 0x80218000;    // ROM offset per asset (moved from 0x800C185C): us.toml
    constexpr uint32_t asset_sizes = 0x80218400;      // size per asset (moved from 0x8001CD64): us.toml
    constexpr uint32_t boxes = 0x80218800;            // 16 bytes per type: {front, rear, half width, height}: us.toml
    constexpr uint32_t table16 = 0x80218C00;          // 16 bytes per type (0x800CADCC, entry 22 special): us.toml
    constexpr uint32_t prefixes = 0x80219000;         // texture name prefix per type (0x800C64C4): us.toml
    constexpr uint32_t no_prefix = 0x80219100;        // "R49X_": matches no texture
    constexpr uint32_t desc_ptrs = 0x80219200;        // physics descriptor pointers (0x800C07BC), 64: us.toml
    constexpr int max_descs = 64;
    constexpr uint32_t tire_curves = 0x80219300;      // 6 floats each: 2049 front, rear
    constexpr uint32_t car_names = 0x80219400;        // part name prefixes of the extra types, 8 bytes each
    constexpr uint32_t logo_names = 0x80219600;       // car select logo names of the extra types, 16 bytes each
    // California Speed's tire curves, front and rear per car (6 floats each: its grip factors folded in).
    constexpr uint32_t calspeed_tire_curves = 0x80219840;
    constexpr uint32_t descs = 0x8021A000;            // the extra types' descriptors (Rush 2's 0xEC-byte layout)
    constexpr uint32_t desc_size = 0xEC;
    constexpr uint32_t side_slots = 0x8021C000;       // player records' options for types 22-56 (car2049.cpp)
    constexpr int side_slot_size = 0x1D0;             // (57 - 22) * 13 = 455, rounded up
    constexpr int max_side_slots = 16;
    constexpr uint32_t guest_side_slots = 0x8021E000; // an online race's other players' side slots, 8
    constexpr int max_guests = 8;
    static_assert(prefixes + types * 4 <= no_prefix);
    static_assert(car_names + (types - rush2_types) * 8 <= logo_names);
    static_assert(logo_names + (types - rush2_types) * 16 <= calspeed_tire_curves);
    static_assert(calspeed_tire_curves + count_calspeed * 2 * 0x18 <= descs);
    static_assert(descs + (count_2049 + count_calspeed) * desc_size <= side_slots);
    static_assert(side_slot_size >= (types - rush2_types) * 13);
    static_assert(side_slots + max_side_slots * side_slot_size <= guest_side_slots);
    static_assert(guest_side_slots + max_guests * side_slot_size <= 0x80222000);
}

#endif
