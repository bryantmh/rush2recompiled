#ifndef __HUD_LAYOUT_H__
#define __HUD_LAYOUT_H__

// The race HUD's layout engine (src/hud_layout.cpp, docs/hud_layout.md). Pure: it works from the race's settings and
// the HUD catalog (include/hud_catalog.h: every part the HUD can draw, from the ROM's tables and the game's callbacks),
// never from what the game drew, so the same code lays out the HUD in the game (src/hud.cpp applies its output to the
// widgets) and in the previewer (tools/hud_preview), and both know exactly where everything goes.
//
// Every element that can appear in this race keeps its place all race, shown or hidden, so nothing moves when
// something appears or disappears, and an element that can't appear (the tachometer with an automatic gearbox) takes
// no room. Where each element goes is a table of rules (src/hud_layout.cpp, `rules`), read top to bottom, each naming
// the elements it is placed against; elements that may overlap are listed (`allowed_overlap`), and check() reports
// any other overlap.

#include <cstdint>
#include <string>
#include <vector>

#include "hud_catalog.h"

namespace rush2::hud_layout {
    using rush2::hud_catalog::Part;
    using rush2::hud_catalog::Rect;
    using rush2::hud_catalog::Role;
    using rush2::hud_catalog::role_name;

    // RT64 rectangle origins (rt64_extended_gbi.h G_EX_ORIGIN_*), in 1/0x400 of the window's width.
    constexpr uint16_t origin_left = 0x0;
    constexpr uint16_t origin_center = 0x200;
    constexpr uint16_t origin_right = 0x400;
    constexpr uint16_t origin_none = 0x800;  // where the 4:3 screen is (its center stays at the window's center)
    constexpr uint16_t origin_left_half = origin_center / 2;                   // middle of the left half
    constexpr uint16_t origin_right_half = origin_center + origin_center / 2;  // middle of the right half

    constexpr int32_t screen_width = 320;
    constexpr int32_t screen_height = 240;

    enum class Arrangement : uint8_t {
        Single,     // one view, or two stacked (the game's own layout: nothing moves)
        SideBySide, // two views side by side
        Quadrants,  // three or four views
    };

    // The screen layouts, which hand placements (Override) are made for. 3 players use P4's (the same quadrants);
    // P3 is never used.
    enum class LayoutId : uint8_t { P1, P2Stacked, P2Side, P3, P4, Count };
    // The race modes hand placements can be made for (Override::mode). Any: every mode; one made for the race's mode
    // is taken over it. Race is a race, circuit or practice, with or without the BATTLE row or the Weapons cheat (one
    // HUD); Stunt the stunt track (the stunt mode is only played there); Battle a Rush 2049 battle arena.
    enum class HudMode : uint8_t { Any, Race, Stunt, Battle, Count };

    // A hand placement made in the HUD editor (tools/hud_preview/editor.py), built in from src/hud_overrides.inc: in
    // one layout, an element goes where it was put instead of where its rule says (elements placed after it are placed
    // against it there), and/or is drawn at another scale, or isn't drawn at all. Any aspect ratio: x is relative to
    // `origin`, as an Element's.
    struct Override {
        // attached_y: attached up and down only (it keeps its own x); attached_x: sideways only (its own y). only_43:
        // used only with the HUD at 4:3 (the Original aspect ratio), taken there over one without it.
        enum : uint8_t { moved = 1, hidden = 2, scaled = 4, attached = 8, attached_y = 16, attached_x = 32, only_43 = 64 };
        LayoutId layout = LayoutId::P1;
        Role role = Role::Other;
        int8_t player = -1;             // -1: shared
        uint8_t group = 0;
        uint8_t flags = 0;
        int32_t x = 0, y = 0;           // moved: its top left corner, 4:3 coordinates relative to origin
        uint16_t origin = origin_none;
        float scale = 1.0f;             // scaled: drawn at this times its rule's scale (about its top left corner;
                                        // the rule's can depend on the resolution: fitting_scale)
        HudMode mode = HudMode::Any;    // the race mode it's for
        // attached: its top left corner (x, y) from the top left corner of this element's (the same player's, or a
        // shared one), wherever that one goes (both ways: taking that one's anchor; or one way, attached_y / _x).
        // Nothing while that one isn't shown.
        Role to_role = Role::Other;
        int8_t to_player = -1;
        uint8_t to_group = 0;
    };
    // The built-in ones (src/hud_overrides.inc).
    const std::vector<Override>& builtin_overrides();
    // Hand placements in src/hud_overrides.inc's format (the HUD editor's: one "{ LayoutId::P4, Role::Deaths, 3, 0, 1,
    // 120, 200, 0x400, 1.000f }," per line, "//" comments) added to `list`; false with `error` (`source`:line) on a
    // bad line.
    bool parse_overrides(const std::string& text, const std::string& source, std::vector<Override>& list,
                         std::string& error);
    // `base` with `mod`'s placements over it: one of mod's replaces base's for the same element, layout and mode (and
    // 4:3 or not); the rest of base's stay (a HUD mod, rush2::hud's mod_overrides).
    std::vector<Override> merge_overrides(const std::vector<Override>& base, const std::vector<Override>& mod);

    struct Config {
        int players = 1;                // players racing, 1-4
        Arrangement arrangement = Arrangement::Single;
        float hud_width = 320.0f;       // how wide RT64 draws the 4:3 HUD (rush2::splitscreen::hud_width)
        bool widescreen = false;        // the aspect ratio isn't Original
        int language = 0;               // 0x800C4618: 0 English, 1 French, 2 German
        bool odometer = false;          // Rush 2049's odometer is on
        int32_t odometer_w = 32, odometer_h = 8, odometer_gap = 2;  // its full size (rush2::odometer2049)
        float small_scale = 0.75f;      // 3-4 players with the odometer: the time and position's scale
        int race_type = 0;              // 0x8010C3E8: 0 race, 1 circuit, 2 stunt, 3 practice
        bool stunt_arena = false;       // the stunt track (slot STUNT1, track 11): no radar, track map or flag
        bool battle = false;            // a Rush 2049 battle arena (src/rush2049/battle.cpp)
        // Rush 2049's battle HUD (rush2::battle::hud_mode): 0 none, 1 a battle arena, 2 a race with the BATTLE row, 3
        // another race with the Weapons cheat.
        int battle_hud = 0;
        bool battle_time_limit = false; // a battle arena with a time limit: Rush 2's time left stays
        bool stunts_all_tracks = false; // the Stunts on All Tracks cheat (0x8010C200): the stunt panel in a race
        bool deaths = false;            // the race's DEATHS option (race_deaths_8010BC98): the skull
        bool mode_option = false;       // the stunt mode's option (0x8010C169): the IT badge
        bool attract = false;           // the attract demo (race_game_mode 0x800FAE6C nonzero)
        // The game's HUD settings (each element's callback hides it while its byte is 0).
        struct Shown {
            bool time = true;       // 0x8010C3F1: race time and lap time box
            bool speed = true;      // 0x8010C434: speedometer
            bool position = true;   // 0x8010C441
            bool radar = true;      // 0x8010C3EE
            bool map = true;        // 0x8010C3E4: track map, its dots and the finish flag
            bool time_left = true;  // 0x8010C3F2, and not the No Race Timer cheat (0x800D9E88) outside a battle
            bool tach = true;       // 0x8010C425: tachometer (manual gearbox)
            bool gear = true;       // 0x8010C44A: gear (manual gearbox)
            bool lap_banner = true; // 0x8010C452: WRONG WAY
        } shown;
        bool manual[4] = {};            // each player's gearbox is manual (tachometer and gear shown)
        const std::vector<Override>* overrides = nullptr;  // hand placements; null: the built-in ones
        bool operator==(const Config&) const;
    };
    LayoutId layout_id(const Config& c);
    const char* layout_name(LayoutId id);  // "p1", "p2_stacked", "p2_side", "p3", "p4"
    HudMode hud_mode(const Config& c);
    const char* mode_name(HudMode m);      // "any", "race", "stunt", "battle"

    // The catalog variant (which parts there are and where the game draws them) a configuration's race has.
    rush2::hud_catalog::Variant variant_of(const Config& c);

    // One element: the parts of one role, player and group. They move and scale together.
    struct Element {
        Role role = Role::Other;
        int player = -1;
        int group = 0;
        std::vector<int> parts;     // indexes into Output::parts
        Rect game;                  // its bounds as the game lays it out (what the layout keeps room for)
        bool possible = false;      // it can appear in this race (it keeps its place shown or hidden)
        float scale = 1.0f;         // drawn at this scale about its top left corner (game.x0, game.y0)
        int32_t x = 0, y = 0;       // its top left corner as drawn, in 4:3 coordinates relative to `origin`
        uint16_t origin = origin_none;
        Rect box;                   // its bounds as drawn on the screen: 4:3 coordinates widened to the HUD's width
                                    // (x < 0 and x >= 320 past the 4:3 screen's edges in widescreen)
        // An element that slides (WRONG WAY: catalog parts with an `end`): its bounds as the game draws it at the
        // slide's start and end, and the origin it has at the end. While it slides its origin goes from `origin` to
        // `origin_end` as far as it has gone, so it moves smoothly from one anchor to the other.
        bool slides = false;
        Rect start, finish;
        uint16_t origin_end = origin_none;
        bool by_hand = false;       // placed or scaled by a hand placement (Override)
        bool hidden = false;        // a hand placement hides it: it isn't drawn and takes no room
        const char* rule = "";      // the rule that placed it
        int spot = -1;              // which of the rule's places it took (0 = the first)
        bool cramped = false;       // none of its places was clear of every other element
    };

    // What the layout does to one part (and the widget built as it).
    struct Place {
        int element = -1;         // its element, or -1 if it's drawn as the game draws it
        bool placed = false;      // moved by (dx, dy) and drawn with `origin`
        int32_t dx = 0, dy = 0;
        uint16_t origin = origin_none;
        uint16_t origin_end = origin_none;  // a sliding part (catalog `end`): the origin where the slide ends; drawn with
                                            // slide_origin() of the two by how far along it is
        bool hidden = false;      // a hand placement hides its element: not drawn
        float scale = 0.0f;       // 0: not scaled; else its rectangles are scaled about (pivot_x, pivot_y) as drawn
        int32_t pivot_x = 0, pivot_y = 0;
    };

    struct OdometerPlace {
        bool valid = false;       // it can be drawn this race
        int32_t x = 0, y = 0;     // top left corner, 4:3 coordinates relative to origin
        uint16_t origin = origin_none;
        float scale = 1.0f;
    };

    // A player's time and position as drawn (screen coordinates widened to the HUD's width), for the battle HUD.
    struct PanelRow {
        bool valid = false;
        int32_t time_x0 = 0, time_x1 = 0, time_y0 = 0, place_x0 = 0, place_x1 = 0, place_y0 = 0, y0 = 0, y1 = 0;
    };

    // Rush 2049's battle HUD for one view (src/rush2049/battle.cpp draws it here), in 4:3 screen pixels; each part keeps
    // its distance from an anchor, a fraction of the window's width (as rush2::hud::set_widget_scale).
    struct BattlePlace {
        bool valid = false;
        float bar_x = 0, bar_y = 0, bar_sx = 1, bar_sy = 1, bar_anchor = 0.5f;  // HEALTHBG's top left corner, its scale
        float weapon_x = 0, weapon_y = 0, weapon_anchor = 0.5f;                // the weapon held's middle
        float ammo_x = 0;                                                      // its ammo's middle (at weapon_y)
        float coin_x = 0, coin_y = 0, coin_anchor = 0.5f;                      // the coin's middle, the kills on it
        float powerup_x = 0, powerup_y = 0, powerup_anchor = 0.5f;             // the power-up's middle
        float digits = 12;                                                     // the ammo and kills' height
        // Hand placements (Override): which are drawn, and the models' scales (1: their size).
        bool show_bar = true, show_weapon = true, show_coin = true, show_powerup = true;
        float weapon_scale = 1, coin_scale = 1, powerup_scale = 1;
    };

    struct Output {
        Config config;
        std::vector<Part> parts;        // the catalog's, for this race (hud_catalog::parts(variant_of(config)))
        std::vector<Place> places;      // one for each part
        std::vector<Element> elements;
        OdometerPlace odometer[4];
        PanelRow panel_rows[4];
        BattlePlace battle[4];
        // 3-4 players in a race: the trick name's box is placed over the stunt scores' (the scores' box is hidden while
        // the trick name's shows).
        bool stunt_swap[4] = {};
        int map = -1;                   // the track map's element, or -1
        bool split = false;             // the views are side by side or in quadrants
        // The element of a role and player (shared ones: player -1), or -1.
        int element(Role role, int player, int group = 0) const;
    };

    Output solve(const Config& config);

    // Where text a HUD callback prints at (x, y) goes: moved with the element whose parts are under it (the smallest
    // within a few pixels), or by itself if there is none. `shown` (one per part, optional): the parts drawn this
    // frame, so text isn't taken by a hidden part over it (the finish overlay's place over the stunt scores).
    struct TextPlace {
        int32_t dx = 0, dy = 0;
        uint16_t origin = origin_none;
        int element = -1;
        bool hidden = false;  // it's on an element hidden by hand: not drawn
    };
    TextPlace place_text(const Output& out, int32_t x, int32_t y, const std::vector<bool>* shown = nullptr);

    // How far a 4:3 x with an origin is drawn from where it would be without one, for HUD width `hud_width`.
    int32_t origin_offset(uint16_t origin, float hud_width);

    // The origin a sliding part (catalog `end`) is drawn with while the game has its top left corner at (x, y), 4:3:
    // place.origin at the part's `at`, place.origin_end at its `end`, in between in proportion (origin_none counts as
    // the center, which it matches). A part that doesn't slide: place.origin.
    uint16_t slide_origin(const Part& part, const Place& place, int32_t x, int32_t y);

    // Problems with a layout: elements that may show together overlapping, elements off the screen or out of their
    // player's part of it, elements none of whose places was clear.
    struct Issue {
        bool error = false;
        std::string text;
    };
    std::vector<Issue> check(const Output& out);
}

namespace rush2::hud {
    // Where player's Rush 2049 battle HUD goes this race (src/hud.cpp: the engine's Output::battle as solved for the
    // race). False if the race has none or the layout isn't known yet.
    bool battle_place(int player, rush2::hud_layout::BattlePlace& out);
}

#endif
