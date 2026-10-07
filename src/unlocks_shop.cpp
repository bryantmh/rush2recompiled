// The unlock system's shop (src/unlocks.cpp): the UNLOCKS row of the Start Game menu.
//
// UNLOCKS takes the RECORDS row's path to the records' profile list (menu screen 2, func_803B23C8), which reads
// UNLOCKS and UNLOCKS FOR <profile> instead of RECORDS and VIEW RECORDS FOR while it leads to the shop, without its
// VIEW TOTALS row (purchases belong to a profile). Its A then leaves the menus as Select Player does for a race (byte 0x800FAE6C = 2), with player 1 set to
// the chosen profile's record, and the track select that opens (game state 1) is replaced: the game switches to the
// car select's state (game state 2), whose frame (func_803B9478) the shop replaces with its own until B goes back
// to the profile list, as the track select's B goes back to the menus.
//
// The shop is its own screen. It keeps what draws the 3D preview and the menus' background:
// - The car select's setup (func_803B81F0: 1 builds the scene once, then each frame turns the car and slides its
//   carousel towards the selected car; 0 tears it down) with the shop's widget table instead of the car select's
//   (func_800604FC), and its background (func_803AA800). Its car list is the shop's cars
//   (rush2::unlocks::filter_car_list), so the shop moves through its cars as the car select does: it sets the
//   carousel's selected index and type, and the setup slides the carousel there. The setup starts on the car the
//   player record has selected (record + 0x31), so the shop selects its car there, and puts the player's own car
//   back when it closes.
// - Tracks and parts show their model (the track select's diorama, or a Rush 2049 engine from asset
//   car2049::parts_asset) as a scene node of the shop's own, made and turned as the track select turns its dioramas.
//   The carousel's cars are shrunk out of sight meanwhile (rush2_unlocks_carousel_car).
// - An engine is shown as Rush 2049's setup screen shows the selected part: lit red (baked into the model,
//   rush2::track2049::convert_parts) and growing by up to a tenth on a 2 Hz triangle wave.
// Its text is drawn by the car select's text callback (func_803BC048), which the shop's widget table uses and which
// draws the shop's text instead while it is open, with the game's fonts.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

#include "rush2.h"
#include "rush2_hooks.h"
#include "car2049.h"
#include "collectibles.h"
#include "engine2049.h"
#include "track2049.h"
#include "unlocks.h"

extern "C" void func_803B81F0(uint8_t* rdram, recomp_context* ctx); // Car select setup (1) / teardown (0).
extern "C" void func_803AA800(uint8_t* rdram, recomp_context* ctx); // Menu background, per frame.
extern "C" void func_80080BD0(uint8_t* rdram, recomp_context* ctx); // Called first in the car select's frame.
extern "C" void func_80064908(uint8_t* rdram, recomp_context* ctx); // Plays a menu sound.
extern "C" void func_800ABE5C(uint8_t* rdram, recomp_context* ctx); // Reloads the menus' assets (track select's B).
extern "C" void func_80086A60(uint8_t* rdram, recomp_context* ctx); // Loads asset $a0.
extern "C" void func_8005BE3C(uint8_t* rdram, recomp_context* ctx); // Model by name ($a0, slots $a1..$a2).
extern "C" void func_8007F91C(uint8_t* rdram, recomp_context* ctx); // New scene matrix.
extern "C" void func_8008035C(uint8_t* rdram, recomp_context* ctx); // New scene node (model, matrix, parent, flags).
extern "C" void func_8005A2D8(uint8_t* rdram, recomp_context* ctx); // Node $a0's position ($a1) and matrix ($a2).
extern "C" void func_80058ED4(uint8_t* rdram, recomp_context* ctx); // Shows node $a0.
extern "C" void func_80088C24(uint8_t* rdram, recomp_context* ctx); // Selects a font.
extern "C" void func_80093FA8(uint8_t* rdram, recomp_context* ctx); // Text setting ($f12), 0 as the car select's.
extern "C" void func_800737E4(uint8_t* rdram, recomp_context* ctx); // Selects a text style.
extern "C" void func_800734AC(uint8_t* rdram, recomp_context* ctx); // x of a string centered on $a1.
extern "C" void func_800732AC(uint8_t* rdram, recomp_context* ctx); // Width of a string.
extern "C" void func_800734E0(uint8_t* rdram, recomp_context* ctx); // Prints a string at (x, y).

namespace {
    using rush2::unlocks::Item;
    using rush2::unlocks::Kind;

    constexpr uint32_t game_state = 0x8010C0D0;     // 0 menus, 1 track select, 2 car select
    constexpr uint32_t player_count = 0x8010C3E2;   // s16
    constexpr uint32_t player_slots = 0x800C2140;   // 0x28 per player: +2 pressed, +4 held, +6 repeated buttons,
    constexpr uint32_t player_slot_size = 0x28;     // +0x24 player record
    constexpr uint32_t record_car = 0x31;           // The record's selected car (func_80097F8C / func_8009E6D0).
    constexpr uint32_t record_section_end = 0x33;   // The record's first checksummed section is +1..+0x33.
    constexpr uint32_t setup_built = 0x803C69C0;    // u8: func_803B81F0 built the car select.
    constexpr uint32_t car_select_widgets = 0x803C69DC;
    constexpr int car_select_widget_count = 0x51;
    constexpr int widget_size = 0x28;
    constexpr uint32_t model_slots = 0x800D5788;    // u8: loaded model slots (func_8005BE3C searches 0..n-1)
    constexpr int car_select_state = 2;
    constexpr int menu_art_asset = 3;

    constexpr uint16_t button_a = 0x8000;
    constexpr uint16_t button_b = 0x4000;
    constexpr uint16_t button_z = 0x2000;
    constexpr uint16_t button_start = 0x1000;
    constexpr uint16_t button_up = 0x0800;
    constexpr uint16_t button_down = 0x0400;
    constexpr uint16_t button_left = 0x0200;
    constexpr uint16_t button_right = 0x0100;
    constexpr uint16_t button_l = 0x0020;
    constexpr uint16_t button_r = 0x0010;

    constexpr int sound_up = 0x3B;
    constexpr int sound_down = 0x44;
    constexpr int sound_change = 0x49;
    constexpr int sound_confirm = 0x31;
    constexpr int sound_back = 0x32;
    constexpr int sound_bought = 0x66;

    // Text: fonts and styles as the car select uses them.
    constexpr int font_title = 0xA;
    constexpr int font_small = 0;
    constexpr int style_title = 0xA;    // green
    constexpr int style_label = 4;      // gray
    constexpr int style_value = 1;      // white
    constexpr int style_flash = 0x14;   // the game's flashing highlight

    constexpr int category_count = 3;
    const Kind category_kinds[category_count] = { Kind::Car, Kind::Track, Kind::Part };
    const char* const category_names[category_count] = { "CARS", "TRACKS", "PARTS" };

    constexpr int message_frames = 120;
    constexpr int glyph_w = 12, glyph_h = 10;  // The button glyphs, smaller than the Controller Setup screen's.

    // The previews: dioramas at the track select's scale times diorama_size, tipped by diorama_tilt radians.
    constexpr float diorama_size = 0.18f;
    constexpr float diorama_tilt = 0.5f;
    constexpr float part_size = 1.8f;
    constexpr float part_tilt = 0.25f;
    constexpr float vertex_units = 16.0f;   // A model's vertices are in 1/16 units.
    // Rush 2049's selected part (overlay 0x8038A400 at 0x8039E830): scale 1 + 0.1 x a triangle wave of the clock at
    // 0x8017A630 (func_800947F0: frac(2t) folded to 0-1, twice a second).
    constexpr float part_pulse = 0.1f, part_pulse_hz = 2.0f;

    // The car select's carousel: player 1's selected index into the car list and its type (func_803B81F0).
    constexpr uint32_t carousel_type = 0x803CB362;  // s8
    constexpr uint32_t carousel_index = 0x803CB364; // s8

    std::atomic<bool> shop_chosen = false;  // UNLOCKS was the Start Game menu's last choice.

    // The records' profile list (menu screen 2): its strings (language 0) and chosen profile.
    constexpr uint32_t list_title = 0x800C4B9C;     // "RECORDS"
    constexpr uint32_t list_no_profile = 0x800C4D70; // "VIEW TOTALS"
    constexpr uint32_t list_profile = 0x800C4D74;    // "VIEW RECORDS FOR"
    constexpr uint32_t list_choice = 0x803D026C;     // s16: profile (pak x 5 + record), -1 = VIEW TOTALS
    constexpr uint32_t list_cursor = 0x803D0224;     // s32: row, -1 = VIEW TOTALS
    constexpr uint32_t list_rows = 0x803D021C;       // s32: profiles
    constexpr uint32_t list_top = 0x803D022C;        // s32: first profile shown
    constexpr uint32_t list_cursor_row = 0x803D0234; // s32: the cursor's row on screen
    constexpr uint32_t leave_menus = 0x800FAE6C;     // u8: 2 = leave the menus for the track select
    constexpr uint32_t pak_records = 0x8004B220;     // 4 paks of 0x2200 bytes: 5 records of 0x6C0
    constexpr uint32_t pak_stride = 0x2200, record_size = 0x6C0;
    constexpr int records_per_pak = 5;
    uint32_t list_strings[3] = {};                   // The shop's replacements, in RDRAM.
    uint32_t list_saved[3] = {};

    struct Shop {
        bool active = false;
        bool pending = false;       // The profile list chose; the shop opens with the track select.
        int profile = -1;           // The chosen profile (pak x 5 + record), or -1 for no profile.
        uint32_t saved_record = 0;  // Player 1's record pointer before the shop.
        int16_t saved_players = 1;
        uint32_t record = 0;        // Player 1's record, whose selected car shows the previewed car.
        int saved_car = 0;          // Its byte as it was.
        int setup_car = -1;         // The car the car select was built on.
        std::string name;           // Player 1's profile, "" without one.
        int category = 0;
        int index[category_count] = {};
        bool confirming = false;
        int message_timer = 0;
        std::string message;
        bool hide_cars = false;     // A track or part is shown: the carousel's cars aren't drawn.
        std::map<std::string, int16_t> nodes;   // The track and part models' nodes, by item id (-1: no model).
        std::string shown;          // The item whose node is shown.
        float angle = 0.0f;
        float clock = 0.0f;         // Seconds, for the part's pulse.
        std::string revved;         // The part whose engine was last revved.
        uint32_t widgets = 0;       // The shop's widget table.
        int widget_count = 0;
        uint32_t scratch = 0;       // RDRAM for strings, a matrix and a position.
        uint32_t scratch_at = 0;
    };
    Shop shop;

    // Where the selected car stands, and so the previews: the origin. func_803B81F0 places each carousel car at its
    // carousel entry (0x80222100 + player x 0x410 + slot x 0x18: x, y, z, target x, angle; passed to func_8008813C,
    // which hands it to func_8005A2D8 as the node's position). Its build (0x803B8DEC-0x803B8E94) sets y = z = 0 and
    // x = 20 x the slot's offset from the selected car, and each frame slides x so the selected car rests at 0.
    // The race's car state (0x801124A0) is not used by the car select: it holds whatever the last race or attract
    // demo left, so it can't place the previews.
    constexpr float preview_pos[3] = { 0.0f, 0.0f, 0.0f };

    constexpr uint32_t scratch_size = 0x1000;
    constexpr uint32_t scratch_matrix = 0xE00; // 9 floats
    constexpr uint32_t scratch_pos = 0xE40;    // 3 floats
    constexpr uint32_t scratch_hidden_matrix = 0xE80; // 9 floats
    constexpr uint32_t scratch_hidden_pos = 0xEC0;    // 3 floats

    uint32_t vram_of(uint8_t* rdram, void* p) {
        return (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
    }

    // Calls a game function from a hook, keeping the hooked function's registers. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0, float f12 = 0.0f) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        ctx->f12.fl = f12;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    int32_t fbits(float f) {
        int32_t i;
        memcpy(&i, &f, 4);
        return i;
    }

    void play_sound(uint8_t* rdram, recomp_context* ctx, int sound) {
        call(rdram, ctx, func_80064908, sound);
    }

    std::vector<const Item*> items_of(Kind kind) {
        std::vector<const Item*> out;
        for (const Item& item : rush2::unlocks::items()) {
            if (item.kind == kind && rush2::unlocks::item_available(item)) {
                out.push_back(&item);
            }
        }
        return out;
    }

    const Item* selected() {
        std::vector<const Item*> list = items_of(category_kinds[shop.category]);
        if (list.empty()) {
            return nullptr;
        }
        int& i = shop.index[shop.category];
        i = std::clamp(i, 0, (int)list.size() - 1);
        return list[i];
    }

    // The categories with something to buy (parts need the 2049 ROM).
    bool category_shown(int c) {
        return !items_of(category_kinds[c]).empty();
    }

    // The shop's widget table: the car select's title bar, info panel, text boxes and arrows (without their car
    // select callbacks), and its text callback.
    void build_widgets(uint8_t* rdram) {
        if (shop.widgets != 0) {
            return;
        }
        struct Pick {
            int index;          // In the car select's table.
            bool keep_callback;
        };
        const Pick picks[] = {
            { 0, true }, { 1, true },                       // TITLELEFT, TITLERIGHT
            { 4, false }, { 5, true }, { 6, true },         // OPTIONBGRIGHT, OPTIONBGRIGHTL, OPTIONBGCAR1
            { 9, false }, { 10, false }, { 11, false }, { 12, false }, // OPTIONTEXTBOX x4
            { 40, true }, { 42, true },                     // BIGARROW: player 1's right and left (mirrored and
                                                            // blinking through their callback, func_803BB414)
            { car_select_widget_count - 1, true },          // the text callback
        };
        int n = (int)(sizeof(picks) / sizeof(picks[0]));
        shop.widgets = vram_of(rdram, recomp::alloc(rdram, n * widget_size));
        for (int k = 0; k < n; k++) {
            uint32_t src = car_select_widgets + picks[k].index * widget_size;
            uint32_t dst = shop.widgets + k * widget_size;
            for (int i = 0; i < widget_size; i += 4) {
                MEM_W(0, (int32_t)(dst + i)) = MEM_W(0, (int32_t)(src + i));
            }
            if (!picks[k].keep_callback) {
                MEM_W(0, (int32_t)(dst + 0x20)) = 0;
            }
        }
        shop.widget_count = n;
    }

    // Strings for the text calls, in RDRAM (rewritten every frame).
    uint32_t str(uint8_t* rdram, const std::string& s) {
        if (shop.scratch_at + s.size() + 1 > scratch_matrix) {
            shop.scratch_at = 0;
        }
        uint32_t at = shop.scratch + shop.scratch_at;
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(at + i)) = i < s.size() ? s[i] : 0;
        }
        shop.scratch_at += (uint32_t)((s.size() + 4) & ~3u);
        return at;
    }

    std::string upper(const std::string& s) {
        std::string out;
        for (char c : s) {
            out.push_back(c >= 'a' && c <= 'z' ? char(c - 32) : c >= 0x20 && c < 0x7F ? c : '?');
        }
        return out;
    }

    int points_left(const std::set<std::string>& bought) {
        return rush2::collectibles::points(shop.name) - rush2::unlocks::spent(bought);
    }

    // The selected car in CARS (the carousel keeps it whichever category is open).
    const Item* selected_car() {
        std::vector<const Item*> list = items_of(Kind::Car);
        if (list.empty()) {
            return nullptr;
        }
        int& i = shop.index[0];
        i = std::clamp(i, 0, (int)list.size() - 1);
        return list[i];
    }

    // Slides the carousel to the selected car: its list is the shop's cars, in the shop's order.
    void scroll_carousel(uint8_t* rdram) {
        const Item* car = selected_car();
        if (car != nullptr) {
            MEM_B(0, (int32_t)carousel_index) = (int8_t)shop.index[0];
            MEM_B(0, (int32_t)carousel_type) = (int8_t)car->value;
        }
    }

    // The record's selected car byte, without marking the record for saving (func_80097F8C would): the low 5 bits,
    // as the game keeps it (the 2049 types' full type comes from shop_car(), src/car2049.cpp). The record's first
    // section (+1..+0x33, holding the byte) keeps a byte sum at +0 that the game checks when it reads the Controller
    // Pak (func_80098190), so it is updated too: a save made meanwhile stays valid.
    void set_record_byte(uint8_t* rdram, int value) {
        MEM_B(0, (int32_t)(shop.record + record_car)) = (int8_t)value;
        uint8_t sum = 0;
        for (uint32_t i = 1; i <= record_section_end; i++) {
            sum += MEM_BU(0, (int32_t)(shop.record + i));
        }
        MEM_B(0, (int32_t)shop.record) = (int8_t)sum;
    }

    void set_record_car(uint8_t* rdram, int type) {
        set_record_byte(rdram, (MEM_BU(0, (int32_t)(shop.record + record_car)) & ~0x1F) | (type & 0x1F));
    }

    void teardown(uint8_t* rdram, recomp_context* ctx) {
        call(rdram, ctx, func_803B81F0, 0);
        shop.nodes.clear();
        shop.shown.clear();
    }

    // An ENGINE level's model, as Rush 2049's setup screen picks it.
    int engine_model(const Item& item) {
        return item.value % rush2::car2049::part_models;
    }

    // The track or part model, as a node of the shop's scene.
    int16_t make_model(uint8_t* rdram, recomp_context* ctx, const Item& item) {
        uint32_t name;
        if (item.kind == Kind::Track) {
            rush2::track2049::prepare_menu_art(rdram);
            call(rdram, ctx, func_80086A60, menu_art_asset, 0);
            name = rush2::track2049::diorama_name(rdram, item.value);
        }
        else {
            call(rdram, ctx, func_80086A60, rush2::car2049::parts_asset, 0);
            name = str(rdram, rush2::car2049::part_model_name(engine_model(item)));
        }
        int slots = MEM_BU(0, (int32_t)model_slots);
        int model = call(rdram, ctx, func_8005BE3C, (int32_t)name, 0, slots - 1, 1);
        if (model < 0) {
            printf("[unlocks] No model for %s\n", item.id);
            fflush(stdout);
            return -1;
        }
        int32_t matrix = call(rdram, ctx, func_8007F91C);
        return (int16_t)call(rdram, ctx, func_8008035C, model, matrix, -1, 0x40);
    }

    // Shrinks a scene node to nothing, well below the previews: the models and the carousel's cars stay shown and are
    // put out of sight (func_8008813C places the cars again each frame). The drop makes the frame interpolation
    // (src/interpolation.cpp) take each swap as a teleport and snap, rather than grow a model out of nothing.
    void shrink_node(uint8_t* rdram, recomp_context* ctx, int16_t node) {
        constexpr float tiny = 1e-4f;
        constexpr float drop = 1000.0f;
        uint32_t m = shop.scratch + scratch_hidden_matrix, pos = shop.scratch + scratch_hidden_pos;
        for (int i = 0; i < 9; i++) {
            MEM_W(0, (int32_t)(m + i * 4)) = fbits(i % 4 == 0 ? tiny : 0.0f);
        }
        for (int i = 0; i < 3; i++) {
            MEM_W(0, (int32_t)(pos + i * 4)) = fbits(preview_pos[i] - (i == 1 ? drop : 0.0f));
        }
        call(rdram, ctx, func_8005A2D8, node, (int32_t)pos, (int32_t)m);
    }

    // Makes every track and part model while the scene is built (loading their assets later, on a category switch,
    // would cost a frame), shown and shrunk out of sight.
    void make_models(uint8_t* rdram, recomp_context* ctx) {
        for (Kind kind : { Kind::Track, Kind::Part }) {
            for (const Item* item : items_of(kind)) {
                if (shop.nodes.contains(item->id)) {
                    continue;
                }
                int16_t node = make_model(rdram, ctx, *item);
                shop.nodes[item->id] = node;
                if (node >= 0) {
                    shrink_node(rdram, ctx, node);
                    call(rdram, ctx, func_80058ED4, node, 0, 1);
                }
            }
        }
    }

    // Puts away the model shown, if any.
    void put_away_model(uint8_t* rdram, recomp_context* ctx) {
        auto old = shop.nodes.find(shop.shown);
        if (old != shop.nodes.end() && old->second >= 0) {
            shrink_node(rdram, ctx, old->second);
        }
        shop.shown.clear();
    }

    // Shows the selected item's model and puts away the one shown before.
    void update_model(uint8_t* rdram, recomp_context* ctx, const Item& item) {
        if (shop.shown != item.id) {
            put_away_model(rdram, ctx);
            shop.shown = item.id;
        }
        auto it = shop.nodes.find(item.id);
        if (it == shop.nodes.end()) {
            return;
        }
        int16_t node = it->second;
        if (node < 0) {
            return;
        }
        // Turned about Y, then tipped towards the camera so a diorama is seen from above.
        float scale = item.kind == Kind::Track ? rush2::track2049::diorama_scale(rdram, item.value) * diorama_size
                                               : part_size;
        float tilt = item.kind == Kind::Track ? diorama_tilt : part_tilt;
        if (item.kind == Kind::Part) {
            float wave = shop.clock * part_pulse_hz;
            wave -= std::floor(wave);
            scale *= 1.0f + part_pulse * 2.0f * std::min(wave, 1.0f - wave);
        }
        float c = std::cos(shop.angle), sn = std::sin(shop.angle), ct = std::cos(tilt), st = std::sin(tilt);
        const float rows[3][3] = {
            { c * scale, sn * st * scale, -sn * ct * scale },
            { 0.0f, ct * scale, st * scale },
            { sn * scale, -c * st * scale, c * ct * scale },
        };
        uint32_t m = shop.scratch + scratch_matrix, pos = shop.scratch + scratch_pos;
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) {
                MEM_W(0, (int32_t)(m + (r * 3 + k) * 4)) = fbits(rows[r][k]);
            }
        }
        // A part turns about its center: the node stands where the center's image lands back on the car's spot.
        std::array<float, 3> center{};
        if (item.kind == Kind::Part) {
            center = rush2::car2049::part_center(engine_model(item));
            for (float& c : center) {
                c /= vertex_units;
            }
        }
        for (int k = 0; k < 3; k++) {
            float moved = center[0] * rows[0][k] + center[1] * rows[1][k] + center[2] * rows[2][k];
            MEM_W(0, (int32_t)(pos + k * 4)) = fbits(preview_pos[k] - moved);
        }
        call(rdram, ctx, func_8005A2D8, node, (int32_t)pos, (int32_t)m);
    }

    void enter(uint8_t* rdram, recomp_context* ctx) {
        if (shop.scratch == 0) {
            shop.scratch = vram_of(rdram, recomp::alloc(rdram, scratch_size));
        }
        build_widgets(rdram);
        shop.active = true;
        shop.confirming = false;
        shop.message_timer = 0;
        shop.hide_cars = false;
        shop.nodes.clear();
        shop.shown.clear();
        shop.revved.clear();
        shop.saved_players = (int16_t)MEM_H(0, (int32_t)player_count);
        MEM_H(0, (int32_t)player_count) = 1;
        shop.saved_record = (uint32_t)MEM_W(0, (int32_t)(player_slots + 0x24));
        shop.record = shop.saved_record;
        shop.name.clear();
        if (shop.profile >= 0) {
            shop.record = pak_records + (shop.profile / records_per_pak) * pak_stride +
                (shop.profile % records_per_pak) * record_size;
            for (int i = 0; i < 13; i++) {
                char c = (char)MEM_BU(0, (int32_t)(shop.record + 2 + i));
                if (c == 0) break;
                shop.name.push_back(c);
            }
            MEM_W(0, (int32_t)(player_slots + 0x24)) = (int32_t)shop.record;
        }
        shop.saved_car = MEM_BU(0, (int32_t)(shop.record + record_car));
        if (!category_shown(shop.category)) {
            shop.category = 0;
        }
        MEM_W(0, (int32_t)game_state) = car_select_state;
    }

    void leave(uint8_t* rdram, recomp_context* ctx) {
        teardown(rdram, ctx);
        set_record_byte(rdram, shop.saved_car);
        shop.active = false;
        MEM_W(0, (int32_t)(player_slots + 0x24)) = (int32_t)shop.saved_record;
        MEM_H(0, (int32_t)player_count) = shop.saved_players;
        MEM_W(0, (int32_t)game_state) = 0;
        call(rdram, ctx, func_800ABE5C);
    }

    void show_message(const std::string& text) {
        shop.message = text;
        shop.message_timer = message_frames;
    }

    // Returns true when the shop closed.
    bool handle_input(uint8_t* rdram, recomp_context* ctx, uint16_t pressed, uint16_t repeat) {
        uint16_t moves = pressed;
        const Item* item = selected();
        std::set<std::string> bought = rush2::collectibles::purchases(shop.name);
        if (shop.confirming) {
            if (pressed & (button_a | button_start)) {
                shop.confirming = false;
                if (item != nullptr && rush2::collectibles::purchase(shop.name, item->id, item->cost,
                                                                      rush2::unlocks::spent(bought))) {
                    play_sound(rdram, ctx, sound_bought);
                    show_message(std::string(item->name) + " UNLOCKED");
                }
                else {
                    play_sound(rdram, ctx, sound_back);
                }
            }
            else if (pressed & button_b) {
                shop.confirming = false;
                play_sound(rdram, ctx, sound_back);
            }
            return false;
        }
        if (pressed & button_b) {
            play_sound(rdram, ctx, sound_back);
            leave(rdram, ctx);
            return true;
        }
        int step_category = 0;
        if (pressed & (button_r | button_down | button_z)) step_category = 1;
        if (pressed & (button_l | button_up)) step_category = -1;
        if (step_category != 0) {
            int c = shop.category;
            for (int k = 0; k < category_count; k++) {
                c = (c + step_category + category_count) % category_count;
                if (category_shown(c)) break;
            }
            if (c != shop.category) {
                shop.category = c;
                play_sound(rdram, ctx, step_category > 0 ? sound_down : sound_up);
            }
        }
        if (moves & (button_left | button_right)) {
            int n = (int)items_of(category_kinds[shop.category]).size();
            if (n > 1) {
                int& i = shop.index[shop.category];
                i = (i + ((moves & button_right) ? 1 : n - 1)) % n;
                play_sound(rdram, ctx, sound_change);
                if (category_kinds[shop.category] == Kind::Car) {
                    scroll_carousel(rdram);
                }
            }
        }
        if ((pressed & (button_a | button_start)) && item != nullptr) {
            const Item* first = rush2::unlocks::prerequisite(*item);
            if (bought.contains(item->id)) {
                play_sound(rdram, ctx, sound_back);
                show_message("ALREADY UNLOCKED");
            }
            else if (first != nullptr && !bought.contains(first->id)) {
                play_sound(rdram, ctx, sound_back);
                show_message(std::string("UNLOCK ") + first->name + " FIRST");
            }
            else if (points_left(bought) < item->cost) {
                play_sound(rdram, ctx, sound_back);
                show_message("NEED " + std::to_string(item->cost - points_left(bought)) + " MORE POINTS");
            }
            else {
                shop.confirming = true;
                play_sound(rdram, ctx, sound_confirm);
            }
        }
        return false;
    }

    // Text

    void set_font(uint8_t* rdram, recomp_context* ctx, int font) {
        call(rdram, ctx, func_80088C24, font);
        call(rdram, ctx, func_80093FA8, 0, 0, 0, 0, 0.0f);
    }

    void print(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, const std::string& s) {
        call(rdram, ctx, func_800737E4, style);
        call(rdram, ctx, func_800734E0, x, y, (int32_t)str(rdram, s));
    }

    void print_centered(uint8_t* rdram, recomp_context* ctx, int style, int cx, int y, const std::string& s) {
        call(rdram, ctx, func_800737E4, style);
        uint32_t at = str(rdram, s);
        int x = (int16_t)call(rdram, ctx, func_800734AC, (int32_t)at, cx);
        call(rdram, ctx, func_800734E0, x, y, (int32_t)at);
    }

    void print_right(uint8_t* rdram, recomp_context* ctx, int style, int right, int y, const std::string& s) {
        call(rdram, ctx, func_800737E4, style);
        uint32_t at = str(rdram, s);
        int w = call(rdram, ctx, func_800732AC, (int32_t)at, -1);
        call(rdram, ctx, func_800734E0, right - w, y, (int32_t)at);
    }

    void draw(uint8_t* rdram, recomp_context* ctx) {
        shop.scratch_at = 0;
        const Item* item = selected();
        std::set<std::string> bought = rush2::collectibles::purchases(shop.name);
        int left = points_left(bought);

        set_font(rdram, ctx, font_title);
        print_centered(rdram, ctx, style_title, 0x68, 19, "UNLOCKS");

        // Categories between the L and R glyphs, then the player and their points.
        using rush2::controls::MenuButton;
        int port = MEM_BU(0, (int32_t)(player_slots + 1));
        auto glyph = [&](MenuButton b, int gx, int gy) {
            rush2::controls::add_menu_glyph(rdram, port, b, gx, gy, glyph_w, glyph_h);
        };
        rush2::controls::begin_menu_glyphs(rdram);
        set_font(rdram, ctx, font_small);
        glyph(MenuButton::L, 20, 50);
        int x = 20 + glyph_w + 6;
        for (int c = 0; c < category_count; c++) {
            if (!category_shown(c)) continue;
            print(rdram, ctx, c == shop.category ? style_title : style_label, x, 52, category_names[c]);
            x += call(rdram, ctx, func_800732AC, (int32_t)str(rdram, category_names[c]), -1) + 10;
        }
        glyph(MenuButton::R, x - 4, 50);
        print_right(rdram, ctx, style_value, 300, 52, shop.name.empty() ? "NO PROFILE" : upper(shop.name));
        print_right(rdram, ctx, style_title, 300, 63, std::to_string(left) + " POINTS");

        if (item == nullptr) {
            rush2::controls::end_menu_glyphs(rdram);
            return;
        }
        bool owned = bought.contains(item->id);
        const Item* first = rush2::unlocks::prerequisite(*item);
        bool waiting = !owned && first != nullptr && !bought.contains(first->id);
        bool can_buy = !owned && !waiting && left >= item->cost;

        // The item's name and game, bottom left.
        set_font(rdram, ctx, font_title);
        print_centered(rdram, ctx, owned ? style_value : style_title, 78, 184, item->name);
        set_font(rdram, ctx, font_small);
        print_centered(rdram, ctx, style_label, 78, 203, item->game);

        // The panel: cost, state, what A does.
        constexpr int label_right = 248, value_x = 256;
        const int rows[4] = { 187, 198, 209, 220 };
        if (shop.confirming) {
            print_right(rdram, ctx, style_label, label_right, rows[0], "BUY");
            print(rdram, ctx, style_value, value_x, rows[0], item->name);
            print_right(rdram, ctx, style_label, label_right, rows[1], "FOR");
            print(rdram, ctx, style_value, value_x, rows[1], std::to_string(item->cost) + " PTS");
            glyph(MenuButton::A, label_right - glyph_w, rows[2] - 2);
            print(rdram, ctx, style_flash, value_x, rows[2], "YES");
            glyph(MenuButton::B, label_right - glyph_w, rows[3] - 2);
            print(rdram, ctx, style_value, value_x, rows[3], "NO");
        }
        else {
            print_right(rdram, ctx, style_label, label_right, rows[0], "COST");
            print(rdram, ctx, style_value, value_x, rows[0], std::to_string(item->cost) + " PTS");
            print_right(rdram, ctx, style_label, label_right, rows[1], "STATUS");
            print(rdram, ctx, owned ? style_title : style_value, value_x, rows[1], owned ? "UNLOCKED" : "LOCKED");
            if (waiting) {
                print_right(rdram, ctx, style_label, label_right, rows[2], "NEEDS");
                print(rdram, ctx, style_value, value_x, rows[2], first->name);
            }
            else {
                print_right(rdram, ctx, style_label, label_right, rows[2], "AFTER");
                print(rdram, ctx, style_value, value_x, rows[2], owned ? "-" : std::to_string(left - item->cost) + " PTS");
            }
            if (can_buy) {
                glyph(MenuButton::A, label_right - glyph_w, rows[3] - 2);
            }
            print(rdram, ctx, can_buy ? style_flash : style_label, value_x, rows[3], owned ? "OWNED" : "BUY");
        }

        if (shop.message_timer > 0) {
            set_font(rdram, ctx, font_title);
            print_centered(rdram, ctx, style_flash, 160, 152, shop.message);
        }
        rush2::controls::end_menu_glyphs(rdram);
    }
}

void rush2::unlocks::set_shop_chosen(bool chosen) {
    shop_chosen = chosen;
}

int rush2::unlocks::shop_car() {
    return shop.active ? shop.setup_car : -1;
}

// Start of func_803ABE0C, the track select's frame. Reached from the profile list in the shop's path, it opens the
// shop instead, before the track select draws anything. Returns true to skip the track select's frame.
extern "C" int rush2_unlocks_track_select(uint8_t* rdram, recomp_context* ctx) {
    if (!shop.pending || shop.active) {
        return 0;
    }
    shop.pending = false;
    enter(rdram, ctx);
    return 1;
}

namespace {
    bool list_for_shop() {
        return shop_chosen && rush2::unlocks::enabled();
    }

    uint32_t alloc_string(uint8_t* rdram, const char* text) {
        size_t n = strlen(text) + 1;
        uint32_t at = vram_of(rdram, recomp::alloc(rdram, (n + 3) & ~3u));
        for (size_t i = 0; i < n; i++) {
            MEM_B(0, (int32_t)(at + i)) = text[i];
        }
        return at;
    }
}

// Start of func_803C2E2C, the profile list's text callback, and before its return (0x803C321C): its title and row
// labels read as the shop's while the list leads to it.
extern "C" void rush2_unlocks_list_text_begin(uint8_t* rdram, recomp_context* ctx) {
    if (!list_for_shop()) {
        return;
    }
    if (list_strings[0] == 0) {
        list_strings[0] = alloc_string(rdram, "UNLOCKS");
        list_strings[1] = alloc_string(rdram, "");
        list_strings[2] = alloc_string(rdram, "UNLOCKS FOR");
    }
    const uint32_t at[3] = { list_title, list_no_profile, list_profile };
    for (int i = 0; i < 3; i++) {
        list_saved[i] = (uint32_t)MEM_W(0, (int32_t)at[i]);
        MEM_W(0, (int32_t)at[i]) = (int32_t)list_strings[i];
    }
}

extern "C" void rush2_unlocks_list_text_end(uint8_t* rdram, recomp_context* ctx) {
    if (!list_for_shop() || list_saved[0] == 0) {
        return;
    }
    const uint32_t at[3] = { list_title, list_no_profile, list_profile };
    for (int i = 0; i < 3; i++) {
        MEM_W(0, (int32_t)at[i]) = (int32_t)list_saved[i];
    }
}

// func_803B23C8 at 0x803B25EC, after its up and down with the cursor in $v1: in the shop's path the cursor stays on
// the profiles (the VIEW TOTALS row above them, -1, is left blank), unless there are none.
extern "C" void rush2_unlocks_list_cursor(uint8_t* rdram, recomp_context* ctx) {
    if (!list_for_shop() || MEM_W(0, (int32_t)list_rows) <= 0 || MEM_W(0, (int32_t)list_cursor) >= 0) {
        return;
    }
    MEM_W(0, (int32_t)list_cursor) = 0;
    MEM_W(0, (int32_t)list_top) = 0;
    MEM_W(0, (int32_t)list_cursor_row) = 0;
    ctx->r3 = 0;
}

// func_803B23C8 at 0x803B2668: A chose a row of the profile list (0x803D026C). In the shop's path it leaves the menus
// for the shop instead of opening the records menu. Returns true to skip to the end of the frame's input.
extern "C" int rush2_unlocks_list_choose(uint8_t* rdram, recomp_context* ctx) {
    if (!list_for_shop()) {
        return 0;
    }
    shop.profile = (int16_t)MEM_H(0, (int32_t)list_choice);
    if (shop.profile < 0) {
        return 1;
    }
    shop.pending = true;
    MEM_B(0, (int32_t)leave_menus) = 2;
    return 1;
}

// Start of func_803B9478, the car select's frame: the shop's frame while it is open. Returns true to skip the car
// select's.
extern "C" int rush2_unlocks_shop_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!shop.active) {
        return 0;
    }
    uint16_t pressed = MEM_HU(0, (int32_t)(player_slots + 2));
    uint16_t repeat = MEM_HU(0, (int32_t)(player_slots + 6));
    for (int p = 0; p < 4; p++) {
        uint32_t slot = player_slots + p * player_slot_size;
        MEM_H(0, (int32_t)(slot + 2)) = 0;
        MEM_H(0, (int32_t)(slot + 4)) = 0;
        MEM_H(0, (int32_t)(slot + 6)) = 0;
    }
    if (shop.message_timer > 0) {
        shop.message_timer--;
    }
    if (handle_input(rdram, ctx, pressed, repeat)) {
        return 1;
    }

    // The scene is built once, on the selected car, whichever category is open.
    const Item* item = selected();
    const Item* car = selected_car();
    bool built = MEM_BU(0, (int32_t)setup_built) != 0;
    if (!built) {
        shop.setup_car = car != nullptr ? car->value : 0;
        set_record_car(rdram, shop.setup_car);
    }
    bool show_model = category_kinds[shop.category] != Kind::Car && item != nullptr;
    shop.hide_cars = show_model;
    call(rdram, ctx, func_80080BD0);
    call(rdram, ctx, func_803B81F0, 1);
    if (!built) {
        make_models(rdram, ctx);
    }
    call(rdram, ctx, func_803AA800);
    shop.angle += 0.02f;
    if (shop.angle > 6.2831853f) {
        shop.angle -= 6.2831853f;
    }
    shop.clock += 1.0f / 60.0f;
    if (shop.clock > 1000.0f) {
        shop.clock = 0.0f;
    }
    if (show_model) {
        update_model(rdram, ctx, *item);
    }
    else if (!shop.shown.empty()) {
        put_away_model(rdram, ctx);
    }
    rush2::engine2049::shop_frame();
    std::string part = item != nullptr && item->kind == Kind::Part ? item->id : "";
    if (part != shop.revved) {
        shop.revved = part;
        if (!part.empty()) {
            rush2::engine2049::shop_rev(rdram, ctx, item->value);
        }
    }
    return 1;
}

// Start of func_800604FC (builds a screen's widgets from table $a2, $a3 entries): the car select's, while the shop
// is open, is the shop's.
extern "C" void rush2_unlocks_widgets(uint8_t* rdram, recomp_context* ctx) {
    if (shop.active && (uint32_t)ctx->r6 == car_select_widgets && shop.widgets != 0) {
        ctx->r6 = (int32_t)shop.widgets;
        ctx->r7 = shop.widget_count;
    }
}

// Start of func_803BC048, the car select's text callback: the shop's text while it is open. Returns true to skip the
// car select's.
extern "C" int rush2_unlocks_text(uint8_t* rdram, recomp_context* ctx) {
    if (!shop.active) {
        return 0;
    }
    draw(rdram, ctx);
    ctx->r2 = 1;
    return 1;
}

// Start of func_8008813C ($a0 = car instance: the race's cars, then 36 + type for the car select's), which places a
// car of the car select's carousel (its scene node handle at 0x80219DD0 + instance x 0x134) each frame: while the
// shop shows a track or part it runs from here, and then the car is shrunk out of sight. Returns true to skip the
// call.
extern "C" void func_8008813C(uint8_t* rdram, recomp_context* ctx);

extern "C" int rush2_unlocks_carousel_car(uint8_t* rdram, recomp_context* ctx) {
    static bool inside = false;
    if (!shop.active || !shop.hide_cars || inside) {
        return 0;
    }
    inside = true;
    call(rdram, ctx, func_8008813C, (int32_t)ctx->r4, (int32_t)ctx->r5, (int32_t)ctx->r6, (int32_t)ctx->r7);
    inside = false;
    constexpr uint32_t car_nodes = 0x80219DD0;
    constexpr uint32_t car_node_stride = 0x134;
    constexpr int instances = 2 * rush2::car2049::types;
    int instance = (int16_t)ctx->r4;
    if (instance >= 0 && instance < instances) {
        // Only the cars near the selected one are loaded; the others' handles are 0, which is the menus' background.
        int16_t node = (int16_t)MEM_W(0, (int32_t)(car_nodes + instance * car_node_stride));
        if (node > 0) {
            shrink_node(rdram, ctx, node);
        }
    }
    return 1;
}
