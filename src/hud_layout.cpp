// The race HUD's layout engine (include/hud_layout.h, docs/hud_layout.md).
//
// solve() takes the HUD catalog's parts for the race (include/hud_catalog.h: every part the HUD can draw and where
// the game draws it), groups them into elements (a player's time, their radar, the shared track map...), decides
// which of them can appear in this race from its settings alone, and places each one by the first rule in `rules`
// that applies to it. A rule lists up to three places (spots), tried in order: the first that's clear of every element
// already placed (except the pairs in `allowed_overlap`) and inside the player's part of the screen is taken, else the
// least bad. Places are named the way a layout is described: the player's top left corner, under the time, between
// the time and the position, beside the radar. Mirrored layouts (the right column, the bottom row of quadrants) come
// from the same rules: "outer" is the side toward the screen's edge, "edge" the screen's top (bottom row: bottom) and
// "mid" the line between the views.
//
// Positions are worked out on the screen as drawn: 4:3 coordinates widened to the HUD's width (an element on a
// window edge moves out with it in widescreen), and each element gets the RT64 origin of what it's placed against
// (the window's edge, the middle of its half, the screen's center), which is how it's drawn.
//
// With one view or two stacked, the HUD stays as the game lays it out: each group of touching elements gets the
// origin of the screen third its center is in, and the odometer is placed.

#include "hud_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rush2::hud_layout {
    int32_t origin_offset(uint16_t origin, float hud_width) {
        float spread = hud_width - (float)screen_width;
        return origin < origin_none ? (int32_t)std::lround((origin / (float)origin_right - 0.5f) * spread) : 0;
    }

    const std::vector<Override>& builtin_overrides() {
        // The HUD editor's file (tools/hud_preview/editor.py writes it): one { layout, role, player, group, flags, x, y,
        // origin, scale } per hand placed element.
        static const std::vector<Override> list = {
#include "hud_overrides.inc"
        };
        return list;
    }

    LayoutId layout_id(const Config& c) {
        // 3 players are 4's quadrants with the last one empty: the same layout.
        if (c.arrangement == Arrangement::Quadrants) return LayoutId::P4;
        if (c.arrangement == Arrangement::SideBySide) return LayoutId::P2Side;
        return c.players >= 2 ? LayoutId::P2Stacked : LayoutId::P1;
    }

    const char* layout_name(LayoutId id) {
        static const char* const names[] = { "p1", "p2_stacked", "p2_side", "p3", "p4" };
        return (int)id < (int)LayoutId::Count ? names[(int)id] : "?";
    }

    HudMode hud_mode(const Config& c) {
        if (c.battle || c.battle_hud == 1) return HudMode::Battle;
        return c.race_type == 2 ? HudMode::Stunt : HudMode::Race;
    }

    const char* mode_name(HudMode m) {
        static const char* const names[] = { "any", "race", "stunt", "battle" };
        return (int)m < (int)HudMode::Count ? names[(int)m] : "?";
    }

    uint16_t slide_origin(const Part& part, const Place& place, int32_t x, int32_t y) {
        if (!part.end.valid() || place.origin == place.origin_end) return place.origin;
        int32_t dx = part.end.x0 - part.at.x0, dy = part.end.y0 - part.at.y0;
        float f = std::abs(dx) >= std::abs(dy) ? (dx != 0 ? (float)(x - part.at.x0) / dx : 0.0f)
                                               : (float)(y - part.at.y0) / dy;
        f = std::clamp(f, 0.0f, 1.0f);
        auto value = [](uint16_t o) { return (float)(o == origin_none ? origin_center : o); };
        if (f <= 0.0f) return place.origin;
        if (f >= 1.0f) return place.origin_end;
        return (uint16_t)std::lround(value(place.origin) + (value(place.origin_end) - value(place.origin)) * f);
    }

    bool Config::operator==(const Config& o) const {
        auto shown_eq = [](const Shown& a, const Shown& b) {
            return a.time == b.time && a.speed == b.speed && a.position == b.position && a.radar == b.radar &&
                a.map == b.map && a.time_left == b.time_left && a.tach == b.tach && a.gear == b.gear &&
                a.lap_banner == b.lap_banner;
        };
        for (int p = 0; p < 4; p++) {
            if (manual[p] != o.manual[p]) return false;
        }
        return players == o.players && arrangement == o.arrangement && hud_width == o.hud_width &&
            widescreen == o.widescreen && language == o.language && odometer == o.odometer &&
            odometer_w == o.odometer_w && odometer_h == o.odometer_h && odometer_gap == o.odometer_gap &&
            small_scale == o.small_scale && race_type == o.race_type && stunt_arena == o.stunt_arena &&
            battle == o.battle && stunts_all_tracks == o.stunts_all_tracks && deaths == o.deaths &&
            mode_option == o.mode_option && attract == o.attract && battle_hud == o.battle_hud &&
            battle_time_limit == o.battle_time_limit && overrides == o.overrides && shown_eq(shown, o.shown);
    }

    rush2::hud_catalog::Variant variant_of(const Config& c) {
        rush2::hud_catalog::Variant v;
        v.players = std::clamp(c.players, 1, 4);
        // The game's view count: 1, or 2 for any split screen (it lays the HUD out for two stacked views; the recomp
        // moves it to the halves and quadrants).
        v.views = v.players == 1 && c.arrangement == Arrangement::Single ? 1 : 2;
        v.language = std::clamp(c.language, 0, 2);
        v.time_shown = c.shown.time;
        return v;
    }

    int Output::element(Role role, int player, int group) const {
        for (size_t i = 0; i < elements.size(); i++) {
            const Element& e = elements[i];
            if (e.role == role && e.player == player && e.group == group) return (int)i;
        }
        return -1;
    }
}

namespace {
    using namespace rush2::hud_layout;
    using rush2::hud_catalog::Kind;
    using rush2::hud_catalog::message_game_over;

    constexpr int32_t inset_x = 12; // overscan inset of the game's 2D scissor
    constexpr int32_t inset_y = 10;
    constexpr int32_t margin = 5;      // a player's HUD from the edges of their half or quadrant
    constexpr int32_t text_margin = 4; // how far text may start outside the part it's printed on

    // ---- Whether an element can appear in this race (it keeps its place from the start, shown or hidden) ----
    // From the elements' callbacks (src/hud_catalog.cpp notes each one): the game's HUD settings and the race decide
    // whether an element can show at all. One that can't takes no room.
    bool possible(const Config& c, Role role, int player, int group) {
        bool manual = player >= 0 && player < 4 && c.manual[player];
        // A battle arena hides Rush 2's HUD but the speedometer (with a manual gearbox's tachometer and gear) and the
        // clock (the time left, with a time limit), and the finish overlay built later (rush2::battle::hud_built clears
        // every other callback).
        if (c.battle) {
            switch (role) {
                case Role::Speed: case Role::Tach: case Role::Gear: case Role::Banner: break;
                case Role::TimeLeft: if (!c.battle_time_limit) return false; break;
                case Role::Message: if (group != message_game_over) return false; break;
                default: return false;
            }
        }
        // The stunt mode (the arenas): only player 1's race time, no lap time or position (800BA608 reads the race type);
        // every player's speedometer, tachometer and gear.
        bool stunt = c.race_type == 2;
        bool laps = !stunt && c.race_type != 3 && !c.attract;
        switch (role) {
            case Role::Time: return c.shown.time && !(stunt && player > 0);
            case Role::LapTime: return c.shown.time && !stunt;
            case Role::Speed: return c.shown.speed;
            case Role::Position: return c.shown.position && !stunt;
            case Role::Radar: return c.shown.radar && !c.stunt_arena;   // hidden on the stunt track (800B920C)
            case Role::Map: return c.shown.map && !c.stunt_arena;       // (800B8188)
            case Role::LapsLeft: return c.shown.map && !c.stunt_arena && laps;  // (800B9D48)
            case Role::TimeLeft: return c.shown.time_left;
            case Role::Tach: return manual && c.shown.tach;
            case Role::Gear: return manual && c.shown.gear;
            case Role::Deaths: return c.deaths && !c.battle;            // (800B9C44)
            case Role::It: return stunt && c.mode_option && !c.attract && !c.battle;  // (800B9CC0)
            case Role::StuntScore: case Role::StuntTrick:
                return (stunt || c.stunts_all_tracks) && !c.battle;     // (80060E34)
            case Role::Odometer: return c.odometer;
            case Role::Message: return true;
            case Role::Banner: return true;
            default: return false;
        }
    }

    bool battle_role(Role r) {
        return r == Role::BattleBar || r == Role::BattleWeapon || r == Role::BattleCoin || r == Role::BattlePowerup;
    }

    // ---- Pairs of elements that may overlap ----
    // The banners and messages come and go over the view; the trick name takes the scores' place while it shows; the
    // tachometer hangs from the speedometer, and its needle at full revs passes under the gear's left edge (as with one
    // player); the finish flag and laps-left number sit on the track map. (The skull and
    // IT badge stay clear of the lap time box: it shows after every checkpoint and would hide them.)
    bool allowed_overlap(Role a, Role b) {
        auto pair = [&](Role x, Role y) { return (a == x && b == y) || (a == y && b == x); };
        return a == Role::Banner || b == Role::Banner || a == Role::Message || b == Role::Message ||
            pair(Role::StuntScore, Role::StuntTrick) ||
            pair(Role::Tach, Role::Speed) || pair(Role::Tach, Role::Gear) || pair(Role::LapsLeft, Role::Map) || (battle_role(a) && battle_role(b));
    }

    // ---- The rules ----
    using Roles = uint32_t;
    constexpr Roles bit(Role r) { return 1u << (int)r; }
    constexpr Roles corner_panel = 1u << 30; // the player's corner panel: the time (left column), the position (right)
    constexpr Roles inner_panel = 1u << 31;  // the other one
    constexpr int8_t game_gap = -1;          // the gap the game leaves between them
    constexpr int8_t odometer_gap = -2;      // rush2::odometer2049::gap

    enum class X : uint8_t {
        Stack,       // the stack's side
        Left, Right, Outer, Inner,  // against that side of the player's part of the screen
        Center,      // centered in it
        Between,     // centered between xref (on its left) and xref2 (on its right)
        AlignOuter,  // its outer edge lined up with xref's
        AlignLeft,   // its left edge lined up with xref's
        Follow,      // keeps the game's offset from xref
        Beside,      // beside xref on its inner side, gap away
        Keep,        // keeps its place in its player's half or quadrant (thirds; move_to_side)
        ScreenCenter,// centered on the screen
        ViewCenter,  // its frame (the speedometer's plate, the tachometer's arc: not where its needle reaches) centered
                     // on its view, anchored at the view's middle (exact at every aspect ratio)
        CenterOn,    // centered on xref
        Pair         // the stunt scores and trick name placed together (stunt_pair; sets y too)
    };
    enum class Y : uint8_t {
        Stack,       // the stack's next place
        Edge, Mid,   // against the screen's top (bottom row: bottom) or the line between the views
        Top, Bottom,
        Center,      // centered in the player's part of the screen
        After,       // past yref toward the line between the views (shared: under it), gap away
        Before,      // past yref toward the screen's edge (shared: over it), gap away
        Follow,      // keeps the game's offset from yref
        CenterOn,    // centered on yref
        AlignEdge,   // its edge toward the screen's top (bottom row: bottom) lined up with yref's
        Keep,
        ScreenMiddle // centered on the line between the top and bottom rows
    };
    // Stacks: elements placed one after another from a corner, each gap from the last.
    enum class Stack : uint8_t {
        None,
        CornerLeft,  // from the top left corner (bottom row: bottom left), down (up)
        CornerRight, // from the top right corner, down
        CornerOuter, // CornerLeft in the left column, CornerRight in the right
        MidOuter,    // from the outer corner on the line between the views, toward the screen's edge
        BottomOuter, // from the bottom outer corner, up
        Count
    };

    struct Spot {
        X x = X::Stack;
        Y y = Y::Stack;
        Stack stack = Stack::None;
        Roles xref = 0, xref2 = 0, yref = 0;
        int8_t gap = 0;
        Roles skip = 0;   // stack items it goes over (and may overlap)
        Roles needs = 0;  // taken only if these can appear
        Roles unless = 0; // taken only if none of these can appear
    };

    // Which layouts a rule is for (all of the bits).
    enum : uint32_t {
        m_single = 1, m_side = 2, m_quads = 4, m_split = 8, m_top = 16, m_bottom = 32, m_arena = 64, m_race = 128,
        m_43 = 256,  // the HUD at 4:3 (the Original aspect ratio)
    };

    struct Rule {
        Role role;
        uint32_t when;
        const char* name;
        Spot spots[3];
    };

    Spot stack(Stack s, int8_t gap = 0, Roles skip = 0, Roles needs = 0) {
        Spot p;
        p.stack = s;
        p.gap = gap;
        p.skip = skip;
        p.needs = needs;
        return p;
    }
    Spot at(X x, Roles xref, Y y, Roles yref, int8_t gap = 0) {
        Spot p;
        p.x = x;
        p.xref = xref;
        p.y = y;
        p.yref = yref;
        p.gap = gap;
        return p;
    }

    // Read top to bottom: an element is placed by the first rule for its role and layout, and can only be placed
    // against elements placed before it.
    const std::vector<Rule>& rules() {
        static const std::vector<Rule> table = [] {
            std::vector<Rule> r;
            const Roles time = bit(Role::Time) | bit(Role::LapTime);
            // Split screen. The track map first (it keeps its place): each player's HUD is placed clear of it.
            r.push_back({ Role::Map, m_split, "track map: centered on the line (quadrants: the cross) between the views",
                          { at(X::Keep, 0, Y::Keep, 0) } });
            r.push_back({ Role::LapsLeft, m_split, "finish flag and laps left: on the track map",
                          { at(X::Follow, bit(Role::Map), Y::Follow, bit(Role::Map)) } });
            // Each player's part of the screen.
            r.push_back({ Role::Time, m_split, "race time: top left corner", { stack(Stack::CornerLeft) } });
            {
                Spot s = stack(Stack::CornerLeft, game_gap);
                s.x = X::Follow;
                s.xref = bit(Role::Time);
                r.push_back({ Role::LapTime, m_split, "lap time box: under the race time (bottom row: over it)", { s } });
            }
            r.push_back({ Role::Position, m_split, "position: top right corner", { stack(Stack::CornerRight) } });
            // The speedometer and tachometer are centered on their view, always; the speedometer at the top, else
            // (no room between the time and position: side by side in 4:3) under them.
            r.push_back({ Role::Speed, m_split | m_top, "speedometer: centered on the view at the top, else under the time and position",
                          { at(X::ViewCenter, 0, Y::Edge, 0),
                            at(X::ViewCenter, 0, Y::After, time | bit(Role::Position), 2) } });
            r.push_back({ Role::Speed, m_split | m_bottom, "speedometer: centered on the view at its top",
                          { at(X::ViewCenter, 0, Y::Mid, 0) } });
            r.push_back({ Role::Tach, m_split, "tachometer: centered on the view, hanging from the speedometer",
                          { at(X::ViewCenter, 0, Y::Follow, bit(Role::Speed)) } });
            r.push_back({ Role::Radar, m_split, "radar: outer side, against the line between the views",
                          { stack(Stack::MidOuter) } });
            {
                // Quadrants: centered on the radar, against it.
                Spot on_radar = stack(Stack::MidOuter, 0, 0, bit(Role::Radar));
                on_radar.x = X::CenterOn;
                on_radar.xref = bit(Role::Radar);
                r.push_back({ Role::Odometer, m_quads | m_race, "odometer: centered on the radar, against it (bottom row: under it), else under the time",
                              { on_radar, stack(Stack::CornerLeft, odometer_gap) } });
            }
            r.push_back({ Role::Odometer, m_side, "odometer: over the radar, else under the time",
                          { stack(Stack::MidOuter, odometer_gap, 0, bit(Role::Radar)), stack(Stack::CornerLeft, odometer_gap) } });
            for (Role badge : { Role::Deaths, Role::It }) {
                // Clear of the lap time box (it shows after every checkpoint): under the corner panel, else beside the
                // radar, centered on it.
                Spot beside_radar = at(X::Beside, bit(Role::Radar) | bit(Role::Odometer), Y::CenterOn, bit(Role::Radar), 5);
                beside_radar.needs = bit(Role::Radar);
                Spot beside = stack(Stack::CornerOuter, 5);
                beside.x = X::Beside;
                beside.xref = bit(Role::Radar) | bit(Role::Odometer);
                r.push_back({ badge, m_split | m_top, badge == Role::Deaths ? "skull: under the corner panel, else beside the radar"
                                                                            : "IT badge: under the corner panel, else beside the radar",
                              { stack(Stack::CornerOuter, 5), beside_radar, beside } });
                Spot beside_panel = at(X::Beside, corner_panel, Y::CenterOn, corner_panel, 5);
                r.push_back({ badge, m_split | m_bottom, badge == Role::Deaths ? "skull: under the radar and odometer, else beside the radar, else beside the corner panel"
                                                                               : "IT badge: under the radar and odometer, else beside the radar, else beside the corner panel",
                              { stack(Stack::MidOuter, 5), beside_radar, beside_panel } });
            }
            {
                // Where one player's game has it: 100 right of the tachometer, 15 higher (scaled with it).
                Spot on_tach = at(X::Follow, bit(Role::Tach), Y::Follow, bit(Role::Tach));
                on_tach.needs = bit(Role::Tach);
                // Without a tachometer (its HUD setting off): inner side, under the top panels.
                Spot under = at(X::AlignOuter, inner_panel, Y::After, time | bit(Role::Position), 5);
                under.unless = bit(Role::Tach);
                Spot under_odometer = at(X::AlignOuter, inner_panel, Y::After, time | bit(Role::Position) | bit(Role::Odometer), 5);
                under_odometer.unless = bit(Role::Tach);
                // A race at 4:3: there's no room beside the tachometer (it would meet the position): its first place,
                // inner side under the top panels.
                r.push_back({ Role::Gear, m_split | m_race | m_43, "gear: inner side, under the top panels (4:3)",
                              { at(X::AlignOuter, inner_panel, Y::After, time | bit(Role::Position), 5),
                                at(X::AlignOuter, inner_panel, Y::After, time | bit(Role::Position) | bit(Role::Odometer), 5) } });
                r.push_back({ Role::Gear, m_split, "gear: beside the tachometer as with one player (always), else inner side under the top panels",
                              { on_tach, under, under_odometer } });
            }
            {
                // The stunt boxes in split screen (a race with Stunts on All Tracks, the stunt track): the scores
                // centered at the bottom of the view; the trick name in their place (there's no room for both: the game shows
                // the trick name instead of the scores while a trick is up, stunt_swap).
                Spot center;
                center.x = X::Pair;
                r.push_back({ Role::StuntScore, m_split, "stunt scores: centered at the bottom of the view (stunt_pair), else bottom outer corner",
                              { center, stack(Stack::BottomOuter) } });
            }
            // Split screen (not one view or two stacked: Rush 2 has both boxes there): the trick name takes the scores'
            // place.
            r.push_back({ Role::StuntTrick, m_split, "trick name: in the stunt scores' place (shown instead of them while a trick is up)",
                          { at(X::AlignOuter, bit(Role::StuntScore), Y::AlignEdge, bit(Role::StuntScore)) } });
            {
                // The stunt track and battle arenas in quadrants (no radar; only player 1 has a race time): under the
                // time, else the bottom outer corner (the stunt track), else (a battle) the top left corner.
                Spot under_time = stack(Stack::CornerLeft, odometer_gap, 0, bit(Role::Time));
                Spot corner = stack(Stack::BottomOuter, odometer_gap, 0, bit(Role::StuntScore));
                r.push_back({ Role::Odometer, m_quads | m_arena, "odometer: under the race time, else bottom outer corner, else top left corner",
                              { under_time, corner, stack(Stack::CornerLeft, odometer_gap) } });
            }
            r.push_back({ Role::Banner, m_split, "place once finished: centered", { at(X::Center, 0, Y::Center, 0) } });
            // Shared.
            r.push_back({ Role::TimeLeft, m_side, "time left: centered over the track map, else at the top",
                          { at(X::ScreenCenter, 0, Y::Before, bit(Role::Map) | bit(Role::LapsLeft), 2),
                            at(X::ScreenCenter, 0, Y::Top, 0) } });
            r.push_back({ Role::TimeLeft, m_quads, "time left: centered over the track map, else on the cross",
                          { at(X::ScreenCenter, 0, Y::Before, bit(Role::Map) | bit(Role::LapsLeft), 2),
                            at(X::ScreenCenter, 0, Y::ScreenMiddle, 0) } });
            // One view or two stacked: the game's layout; only the odometer is placed.
            {
                Spot over_radar = at(X::AlignOuter, bit(Role::Radar), Y::Before, bit(Role::Radar), odometer_gap);
                over_radar.needs = bit(Role::Radar);
                r.push_back({ Role::Odometer, m_single, "odometer: over the radar, else under the time",
                              { over_radar, at(X::AlignLeft, bit(Role::Time), Y::After, time, odometer_gap) } });
            }
            return r;
        }();
        return table;
    }

    // ---- Geometry ----
    struct Area {
        int32_t x0, y0, x1, y1;
    };
    constexpr Area stacked_area[2] = {
        { inset_x, inset_y, screen_width - inset_x, screen_height / 2 - 1 },
        { inset_x, screen_height / 2 + 1, screen_width - inset_x, screen_height - inset_y },
    };
    constexpr Area side_area[2] = {
        { inset_x, inset_y, screen_width / 2 - 1, screen_height - inset_y },
        { screen_width / 2 + 1, inset_y, screen_width - inset_x, screen_height - inset_y },
    };
    constexpr Area quad_area[4] = {
        { inset_x, inset_y, screen_width / 2 - 1, screen_height / 2 - 1 },
        { screen_width / 2 + 1, inset_y, screen_width - inset_x, screen_height / 2 - 1 },
        { inset_x, screen_height / 2 + 1, screen_width / 2 - 1, screen_height - inset_y },
        { screen_width / 2 + 1, screen_height / 2 + 1, screen_width - inset_x, screen_height - inset_y },
    };

    // Origin for an x (4:3) by the screen third it falls in.
    uint16_t origin_for_x(int32_t x) {
        if (3 * x < screen_width) return origin_left;
        if (3 * x > 2 * screen_width) return origin_right;
        return origin_none;
    }
    int third_of(int32_t pos, int32_t a0, int32_t a1) {
        int32_t rel = 3 * (pos - a0);
        return rel < (a1 - a0) ? 0 : rel > 2 * (a1 - a0) ? 2 : 1;
    }
    // Origin for an x (4:3) split: its half's outer edge, middle or inner edge (the center).
    uint16_t side_origin_for_x(int32_t x) {
        static constexpr uint16_t origins[2][3] = {
            { origin_left, origin_left_half, origin_none },
            { origin_none, origin_right_half, origin_right },
        };
        int player = x < screen_width / 2 ? 0 : 1;
        return origins[player][third_of(x, side_area[player].x0, side_area[player].x1)];
    }
    int32_t move_along(int32_t c0, int32_t c1, int32_t from0, int32_t from1, int32_t to0, int32_t to1) {
        switch (third_of((c0 + c1) / 2, from0, from1)) {
            case 0: return to0 - from0;
            case 2: return to1 - from1;
            default: return (to0 + to1) / 2 - (from0 + from1) / 2;
        }
    }
    int stacked_player(int32_t y0, int32_t y1) {
        if (y1 <= stacked_area[0].y1) return 0;
        if (y0 >= stacked_area[1].y0) return 1;
        return -1;
    }
    int32_t move_vertically(int32_t c0, int32_t c1, const Area& from, const Area& to) {
        int32_t center = c0 + c1;
        int32_t middle = from.y0 + from.y1;
        int32_t band = (from.y1 - from.y0) / 4;
        if (center < middle - band) return to.y0 - from.y0;
        if (center > middle + band) return to.y1 - from.y1;
        return (to.y0 + to.y1) / 2 - (from.y0 + from.y1) / 2;
    }
    // How far an element moves from where the game puts it for two stacked views to the same place in its player's
    // half or quadrant (players 3 and 4's start out as copies of player 2's, in the bottom half): near an edge it
    // keeps its distance from that edge, in the middle it stays centered. Shared ones (crossing between the stacked
    // halves) are centered on the line between the halves, or on the cross between the quadrants.
    void move_to_side(const Rect& r, bool quads, int player, int32_t& dx, int32_t& dy) {
        if (player < 0) {
            player = stacked_player(r.y0, r.y1);
        }
        if (player < 0) {
            int32_t cy = quads ? screen_height / 2 : (r.y0 + r.y1 + 1) / 2;
            dx = screen_width / 2 - (r.x0 + r.x1 + 1) / 2;
            dy = cy - (r.y0 + r.y1 + 1) / 2;
            return;
        }
        const Area& from = stacked_area[player == 0 ? 0 : 1];
        const Area& to = quads ? quad_area[std::min(player, 3)] : side_area[std::min(player, 1)];
        dx = move_along(r.x0, r.x1, from.x0, from.x1, to.x0, to.x1);
        dy = move_vertically(r.y0, r.y1, from, to);
    }
    // A player's half (side by side) or quadrant out to the screen's edges.
    Area split_edges(int player, bool quads) {
        bool right = (player & 1) != 0;
        bool bottom = quads && player >= 2;
        Area a;
        a.x0 = right ? screen_width / 2 + 1 : 0;
        a.x1 = right ? screen_width - 1 : screen_width / 2 - 1;
        a.y0 = bottom ? screen_height / 2 + 1 : 0;
        a.y1 = quads && !bottom ? screen_height / 2 - 1 : screen_height - 1;
        return a;
    }
    int32_t scaled_about(int32_t v, int32_t pivot, float scale) {
        return pivot + (int32_t)std::lround((float)(v - pivot) * scale);
    }
    Rect scaled(const Rect& r, float s) {
        if (s == 1.0f || !r.valid()) return r;
        return Rect{ r.x0, r.y0, scaled_about(r.x1 + 1, r.x0, s) - 1, scaled_about(r.y1 + 1, r.y0, s) - 1 };
    }
    int64_t area_of(const Rect& r) {
        return r.valid() ? (int64_t)r.w() * r.h() : 0;
    }
    int64_t overlap_area(const Rect& a, const Rect& b) {
        if (!a.overlaps(b)) return 0;
        return (int64_t)(std::min(a.x1, b.x1) - std::max(a.x0, b.x0) + 1) * (std::min(a.y1, b.y1) - std::max(a.y0, b.y0) + 1);
    }
    bool touching(const Rect& a, const Rect& b) {
        return a.x0 <= b.x1 + 1 && b.x0 <= a.x1 + 1 && a.y0 <= b.y1 + 1 && b.y0 <= a.y1 + 1;
    }
    int find_root(std::vector<int>& parent, int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }

    // Roles placed by rules, one element per player (or shared) and group.
    bool placed_role(Role r) {
        return r != Role::Other;
    }

    struct Solver {
        const Config& c;
        Output& out;
        bool quads;
        bool split;
        std::vector<int> stacks[4][(int)Stack::Count];
        std::vector<int> placed;  // elements placed so far, in order

        Solver(const Config& config, Output& o) : c(config), out(o) {
            quads = c.arrangement == Arrangement::Quadrants;
            split = c.arrangement != Arrangement::Single;
        }

        int32_t offset(uint16_t origin) const {
            return origin_offset(origin, c.hud_width);
        }

        // The element of a role for a player (shared ones from -1), group 0.
        int by_role(int player, Role role) const {
            for (size_t i = 0; i < out.elements.size(); i++) {
                const Element& e = out.elements[i];
                if (e.role == role && e.player == player && e.group == 0) return (int)i;
            }
            return -1;
        }

        // ---- Hand placements (the HUD editor's) ----
        const Override* override_of(const Element& e) const {
            const std::vector<Override>& list = c.overrides != nullptr ? *c.overrides : builtin_overrides();
            // The race mode's own over every mode's; of those, one for 4:3 (only_43, at 4:3) over one for any aspect.
            LayoutId id = layout_id(c);
            HudMode mode = hud_mode(c);
            bool is_43 = c.hud_width <= 320.5f;
            const Override* best = nullptr;
            int best_rank = -1;
            for (const Override& o : list) {
                if (o.layout != id || o.role != e.role || o.player != e.player || o.group != e.group) continue;
                if ((o.flags & Override::only_43) && !is_43) continue;
                if (o.mode != mode && o.mode != HudMode::Any) continue;
                int rank = (o.mode == mode ? 2 : 0) + ((o.flags & Override::only_43) ? 1 : 0);
                if (rank > best_rank) {
                    best = &o;
                    best_rank = rank;
                }
            }
            return best;
        }
        // The odometer's size at a scale as rush2::odometer2049::draw draws it: shrunk, each digit a whole number of
        // pixels.
        Rect odometer_size(float s) const {
            int32_t h = (int32_t)std::lround(c.odometer_h * s);
            int32_t w = c.odometer_h > 0 ? h * c.odometer_w / c.odometer_h : 0;
            return Rect{ 0, 0, w - 1, h - 1 };
        }
        // Places an element by its hand placement, if it has one that moves it (its scale already set).
        bool place_by_hand(Element& e, const Rect& size) {
            const Override* o = override_of(e);
            if (o == nullptr || !(o->flags & Override::moved)) return false;
            e.x = o->x;
            e.y = o->y;
            e.origin = o->origin;
            e.origin_end = o->origin;
            e.box = Rect{ e.x + offset(e.origin), e.y, e.x + offset(e.origin) + size.w() - 1, e.y + size.h() - 1 };
            e.spot = 0;
            e.cramped = false;
            e.by_hand = true;
            e.rule = "placed by hand (HUD editor)";
            return true;
        }
        float hand_scale(const Element& e, float scale) const {
            const Override* o = override_of(e);
            return o != nullptr && (o->flags & Override::scaled) ? o->scale * scale : scale;
        }

        // ---- Elements ----
        int add_element(Role role, int player, int group) {
            Element e;
            e.role = role;
            e.player = player;
            e.group = group;
            out.elements.push_back(e);
            return (int)out.elements.size() - 1;
        }

        void build_elements() {
            const auto& parts = out.parts;
            // Parts of one role, player and group form an element; text and parts that never show don't.
            for (size_t i = 0; i < parts.size(); i++) {
                const Part& p = parts[i];
                if (!p.present || !placed_role(p.role) || !p.reach.valid()) continue;
                int e = -1;
                for (size_t k = 0; k < out.elements.size(); k++) {
                    const Element& el = out.elements[k];
                    if (el.role == p.role && el.player == p.player && el.group == p.group) {
                        e = (int)k;
                        break;
                    }
                }
                if (e < 0) e = add_element(p.role, p.player, p.group);
                out.elements[e].parts.push_back((int)i);
                out.places[i].element = e;
            }
            // Bounds: the frames' opaque texels and everywhere a moving part can go; an element without a frame,
            // all of its parts.
            for (Element& e : out.elements) {
                for (int i : e.parts) {
                    const Part& p = parts[i];
                    if (p.frame) e.game.add(p.opaque);
                    if (!(p.reach == p.at)) e.game.add(p.reach);
                    e.slides = e.slides || p.end.valid();
                }
                if (e.slides) {
                    for (int i : e.parts) {
                        e.start.add(parts[i].at);
                        e.finish.add(parts[i].end.valid() ? parts[i].end : parts[i].at);
                    }
                }
                if (!e.game.valid()) {
                    for (int i : e.parts) e.game.add(parts[i].reach);
                }
                e.possible = e.game.valid() && possible(c, e.role, e.player, e.group);
            }
            // The odometer (drawn by the recomp, no widgets): every player's (in the stunt mode too, where only player
            // 1 has a race time); none in a battle arena (it hides Rush 2's HUD).
            if (c.odometer) {
                for (int p = 0; p < std::clamp(c.players, 1, 4); p++) {
                    int e = add_element(Role::Odometer, p, 0);
                    Element& o = out.elements[e];
                    o.possible = possible(c, Role::Odometer, p, 0);
                    o.game = Rect{ 0, 0, c.odometer_w - 1, c.odometer_h - 1 };  // full size: scale_of scales it
                }
            }
            // Hidden by hand: not drawn, and takes no room.
            for (Element& e : out.elements) {
                const Override* o = override_of(e);
                if (o != nullptr && (o->flags & Override::hidden)) {
                    e.hidden = true;
                    e.possible = false;
                }
            }
        }

        // ---- Scale ----
        // In quadrants (3 or 4 players) the skull, IT badge and stunt boxes are drawn at half size, everything else at
        // small_scale (rush2::hud's fitting_scale, the lap timer's: the scale nearest 75% at which a texel covers
        // whole output pixels).
        float scale_of(const Element& e) const {
            // The gear is drawn at its tachometer's size (it sits beside it as with one player).
            if (e.role == Role::Gear) {
                const Element* tach = ref(e.player, Role::Tach);
                if (tach != nullptr) return tach->scale;
            }
            // Split screen: the trick name is drawn in the stunt scores' place, at their size.
            if (e.role == Role::StuntTrick && split) {
                const Element* scores = ref(e.player, Role::StuntScore);
                if (scores != nullptr) return scores->scale;
            }
            if (!quads) return 1.0f;
            switch (e.role) {
                case Role::Deaths: case Role::It: case Role::StuntScore: case Role::StuntTrick:
                    return 0.5f;
                default:
                    return c.small_scale;
            }
        }

        // ---- Placing ----
        struct Where {
            bool ok = false;
            int32_t x = 0, y = 0;  // 4:3, relative to origin
            uint16_t origin = origin_none;
        };

        uint32_t media(const Element& e) const {
            uint32_t m = 0;
            if (!split) m |= m_single;
            else {
                m |= m_split | (quads ? m_quads : m_side);
                m |= quads && e.player >= 2 ? m_bottom : m_top;
            }
            m |= c.race_type == 2 ? m_arena : m_race;
            if (c.hud_width <= 320.5f) m |= m_43;
            return m;
        }

        const Element* ref(int player, Role which) const {
            int e = by_role(player, which);
            if (e < 0) return nullptr;
            const Element& el = out.elements[e];
            if (!el.possible || el.spot < 0) return nullptr;
            return &el;
        }
        // The placed elements a mask names, for a player (shared ones too).
        std::vector<const Element*> refs(int player, Roles mask) const {
            std::vector<const Element*> v;
            bool right = (player & 1) != 0;
            if (mask & corner_panel) mask |= bit(right ? Role::Position : Role::Time);
            if (mask & inner_panel) mask |= bit(right ? Role::Time : Role::Position);
            for (int r = 0; r < (int)Role::Count; r++) {
                if (!(mask & bit((Role)r))) continue;
                const Element* e = ref(player, (Role)r);
                if (e == nullptr && player >= 0) e = ref(-1, (Role)r);
                if (e != nullptr) v.push_back(e);
            }
            return v;
        }
        static Rect union_of(const std::vector<const Element*>& v) {
            Rect u;
            for (const Element* e : v) u.add(e->box);
            return u;
        }

        Stack resolve(Stack s, int player) const {
            if (s == Stack::CornerOuter) return (player & 1) ? Stack::CornerRight : Stack::CornerLeft;
            return s;
        }

        // How far an element following ref keeps from it (before scaling): the game's offset, but a gear on its
        // tachometer keeps player 1's (the ROM table entries for players 2-4's, 56 below theirs, aren't where the game
        // draws them).
        int32_t follow_offset(const Element& e, const Element& ref, bool x) const {
            const Element* a = &e;
            const Element* b = &ref;
            if (e.role == Role::Gear && ref.role == Role::Tach) {
                int g = by_role(0, Role::Gear), t = by_role(0, Role::Tach);
                if (g >= 0 && t >= 0) {
                    a = &out.elements[g];
                    b = &out.elements[t];
                }
            }
            return x ? a->game.x0 - b->game.x0 : a->game.y0 - b->game.y0;
        }

        Where place(const Element& e, const Spot& s) {
            Where w;
            int p = e.player;
            bool right = p >= 0 && (p & 1) != 0;
            bool bottom = quads && p >= 2;
            bool shared = p < 0;
            Area a = shared || !split ? Area{ 0, 0, screen_width - 1, screen_height - 1 } : split_edges(p, quads);
            Area ct{ a.x0 + margin, a.y0 + margin, a.x1 - margin, a.y1 - margin };
            int32_t W = e.box.w(), H = e.box.h();
            int8_t gap = s.gap == odometer_gap ? (int8_t)c.odometer_gap : s.gap;
            const Element* stack_prev = nullptr;
            if (s.unless) {
                for (int r = 0; r < (int)Role::Count; r++) {
                    if ((s.unless & bit((Role)r)) && ref(p, (Role)r) != nullptr) return w;
                }
            }
            if (s.needs) {
                for (int r = 0; r < (int)Role::Count; r++) {
                    if ((s.needs & bit((Role)r)) && ref(p, (Role)r) == nullptr) return w;
                }
            }
            auto edge_origin = [&](bool on_right) -> uint16_t {
                if (!split || shared) return on_right ? origin_right : origin_left;
                return on_right == right ? (right ? origin_right : origin_left) : origin_none;
            };

            // Stack: where its last item is.
            Stack st = resolve(s.stack, p);
            bool down = true; // the stack grows down
            Rect last;
            if (st != Stack::None && p >= 0 && p < 4) {
                down = st == Stack::CornerLeft || st == Stack::CornerRight ? !bottom : st == Stack::MidOuter ? bottom : false;
                for (int i : stacks[p][(int)st]) {
                    const Element& item = out.elements[i];
                    if (s.skip & bit(item.role)) continue;
                    last.add(item.box);
                    stack_prev = &item;
                }
            }
            if (gap == game_gap) {
                gap = 0;
                if (stack_prev != nullptr) {
                    const Rect& g = e.game;
                    const Rect& pg = stack_prev->game;
                    int32_t d = g.y0 > pg.y1 ? g.y0 - pg.y1 - 1 : pg.y0 - g.y1 - 1;
                    gap = (int8_t)std::max<int32_t>(0, (int32_t)std::lround(d * e.scale));
                }
            }

            // x
            X xk = s.x;
            if (xk == X::Stack) {
                switch (st) {
                    case Stack::CornerLeft: xk = X::Left; break;
                    case Stack::CornerRight: xk = X::Right; break;
                    default: xk = X::Outer; break;
                }
            }
            if (xk == X::Outer) xk = right ? X::Right : X::Left;
            if (xk == X::Inner) xk = right ? X::Left : X::Right;
            switch (xk) {
                case X::Left:
                    w.origin = edge_origin(false);
                    w.x = ct.x0;
                    break;
                case X::Right:
                    w.origin = edge_origin(true);
                    w.x = ct.x1 - W + 1;
                    break;
                case X::Center:
                    w.origin = split && !shared ? (right ? origin_right_half : origin_left_half) : origin_none;
                    w.x = (a.x0 + a.x1 + 1) / 2 - W / 2;
                    break;
                case X::Between: {
                    Rect l = union_of(refs(p, s.xref)), r = union_of(refs(p, s.xref2));
                    if (!l.valid() || !r.valid()) return w;
                    w.origin = right ? origin_right_half : origin_left_half;
                    w.x = (l.x1 + 1 + r.x0) / 2 - W / 2 - offset(w.origin);
                    break;
                }
                case X::Pair:
                    return stunt_pair(e);
                case X::AlignOuter: case X::AlignLeft: case X::Follow: case X::Beside: case X::CenterOn: {
                    auto v = refs(p, s.xref);
                    if (v.empty()) return w;
                    Rect u = union_of(v);
                    w.origin = v[0]->origin;
                    int32_t fx;
                    bool outer_right = split && !shared ? right : (u.x0 + u.x1) / 2 > screen_width / 2;
                    if (xk == X::AlignOuter) fx = outer_right ? u.x1 - W + 1 : u.x0;
                    else if (xk == X::AlignLeft) fx = u.x0;
                    else if (xk == X::Follow) fx = v[0]->box.x0 + (int32_t)std::lround(follow_offset(e, *v[0], true) * v[0]->scale);
                    else if (xk == X::CenterOn) fx = (u.x0 + u.x1 + 1) / 2 - W / 2;
                    else fx = right ? u.x0 - gap - W : u.x1 + 1 + gap;
                    w.x = fx - offset(w.origin);
                    if (!split) w.origin = origin_for_x((v[0]->game.x0 + v[0]->game.x1) / 2), w.x = fx - offset(w.origin);
                    break;
                }
                case X::Keep: {
                    // A scaled element keeps its center.
                    int32_t dx = 0, dy = 0;
                    if (split) move_to_side(e.game, quads, p, dx, dy);
                    w.x = e.game.x0 + dx + (e.game.w() - W) / 2;
                    w.origin = split ? side_origin_for_x((e.game.x0 + e.game.x1) / 2 + dx) : origin_for_x((e.game.x0 + e.game.x1) / 2);
                    break;
                }
                case X::ViewCenter: {
                    if (!split || shared) return w;
                    // Its frame (the opaque texels of its frame parts), scaled about its top left corner.
                    Rect f;
                    for (int i : e.parts) {
                        if (out.parts[i].frame) f.add(out.parts[i].opaque);
                    }
                    if (!f.valid()) f = e.game;
                    float fx0 = (float)(f.x0 - e.game.x0) * e.scale, fw = (float)f.w() * e.scale;
                    // The view's middle: a quarter of the window's width in (three quarters on the right), 80 (240) on
                    // the 4:3 screen.
                    w.origin = right ? origin_right_half : origin_left_half;
                    float middle = right ? 240.0f : 80.0f;
                    w.x = (int32_t)std::lround(middle - fx0 - fw * 0.5f);
                    break;
                }
                case X::ScreenCenter:
                    w.origin = origin_none;
                    w.x = e.game.x0 + screen_width / 2 - (e.game.x0 + e.game.x0 + W) / 2;
                    break;
                default:
                    return w;
            }

            // y
            Y yk = s.y;
            int32_t y_edge = bottom ? ct.y1 - H + 1 : ct.y0;
            int32_t y_mid = bottom ? ct.y0 : ct.y1 - H + 1;
            switch (yk) {
                case Y::Stack:
                    if (!last.valid()) {
                        if (st == Stack::BottomOuter) w.y = ct.y1 - H + 1;
                        else if (st == Stack::MidOuter) w.y = y_mid;
                        else w.y = y_edge;
                    }
                    else {
                        w.y = down ? last.y1 + 1 + gap : last.y0 - gap - H;
                    }
                    break;
                case Y::Edge: w.y = y_edge; break;
                case Y::Mid: w.y = y_mid; break;
                case Y::Top: w.y = ct.y0; break;
                case Y::Bottom: w.y = ct.y1 - H + 1; break;
                case Y::Center: w.y = (a.y0 + a.y1 + 1) / 2 - H / 2; break;
                case Y::ScreenMiddle: w.y = screen_height / 2 - H / 2; break;
                case Y::After: case Y::Before: case Y::Follow: case Y::CenterOn: case Y::AlignEdge: {
                    auto v = refs(p, s.yref);
                    if (v.empty()) return w;
                    Rect u = union_of(v);
                    bool toward_mid_is_down = !bottom; // shared and single: down
                    if (yk == Y::After) w.y = toward_mid_is_down ? u.y1 + 1 + gap : u.y0 - gap - H;
                    else if (yk == Y::Before) w.y = toward_mid_is_down ? u.y0 - gap - H : u.y1 + 1 + gap;
                    else if (yk == Y::Follow) w.y = v[0]->box.y0 + (int32_t)std::lround(follow_offset(e, *v[0], false) * v[0]->scale);
                    else if (yk == Y::CenterOn) w.y = (u.y0 + u.y1 + 1) / 2 - H / 2;
                    else w.y = bottom ? u.y1 - H + 1 : u.y0;
                    break;
                }
                case Y::Keep: {
                    int32_t dx = 0, dy = 0;
                    if (split) move_to_side(e.game, quads, p, dx, dy);
                    w.y = e.game.y0 + dy + (e.game.h() - H) / 2;
                    break;
                }
            }
            w.ok = true;
            return w;
        }

        // The stunt scores in split screen (X::Pair): at the bottom of the view (the top row: on the line between the
        // views, 5 px margin), centered on the view, moved only as far as needed to keep 2 px (else 0) from the
        // elements there (the race time and position, the radar, the track map). The trick name takes their place.
        Where stunt_pair(const Element& e) {
            Where none;
            int p = e.player;
            if (p < 0 || p > 3 || e.role != Role::StuntScore) return none;
            bool right = (p & 1) != 0;
            Area a = split_edges(p, quads);
            // The view's bounds as drawn (its outer edge with the window's), as badness has them.
            Rect lim{ right ? a.x0 : a.x0 + offset(origin_left), a.y0, right ? a.x1 + offset(origin_right) : a.x1, a.y1 };
            int32_t row_y1 = a.y1 - margin;
            int32_t mid = (lim.x0 + lim.x1) / 2;
            uint16_t origin = right ? origin_right_half : origin_left_half;
            int32_t W = e.box.w(), H = e.box.h();
            for (int32_t gap : { 2, 0 }) {
                int32_t L = lim.x0 + margin, R = lim.x1 - margin;
                for (int i : placed) {
                    const Element& o = out.elements[i];
                    if (&o == &e || !o.possible || o.spot < 0 || !o.box.valid()) continue;
                    if (allowed_overlap(Role::StuntScore, o.role) || battle_role(o.role)) continue;
                    if (o.player >= 0 && o.player != p) continue;
                    if (o.box.y1 < row_y1 - H + 1 || o.box.y0 > row_y1) continue;
                    if (o.box.x1 < mid) L = std::max(L, o.box.x1 + 1 + gap);
                    else if (o.box.x0 > mid) R = std::min(R, o.box.x0 - 1 - gap);
                    else R = L - 1;  // something on the middle: no room
                }
                if (R - L + 1 < W) continue;
                int32_t x0 = std::clamp(mid - W / 2, L, R - W + 1);
                return Where{ true, x0 - offset(origin), row_y1 - H + 1, origin };
            }
            return none;
        }

        // How badly a box sits: overlap with the elements already placed (but the allowed pairs and `skip`), and
        // how much of it is outside its player's part of the screen.
        int64_t badness(const Element& e, const Rect& box, Roles skip) const {
            int64_t bad = 0;
            for (int i : placed) {
                const Element& o = out.elements[i];
                if (&o == &e || !o.possible || allowed_overlap(e.role, o.role) || (skip & bit(o.role))) continue;
                if (e.player >= 0 && o.player >= 0 && o.player != e.player) continue;
                bad += overlap_area(box, o.box);
            }
            if (split && e.player >= 0) {
                Area a = split_edges(e.player, quads);
                bool right = (e.player & 1) != 0;
                Rect lim{ right ? a.x0 : a.x0 + offset(origin_left), a.y0, right ? a.x1 + offset(origin_right) : a.x1, a.y1 };
                bad += area_of(box) - overlap_area(box, lim);
            }
            return bad;
        }

        void place_element(int index, const Rule* rule) {
            Element& e = out.elements[index];
            e.scale = hand_scale(e, scale_of(e));
            e.by_hand = e.scale != scale_of(e);
            Rect size = e.role == Role::Odometer ? odometer_size(e.scale) : scaled(e.game, e.scale);
            if (place_by_hand(e, size)) {
                placed.push_back(index);
                return;
            }
            e.box = size; // its size, for the spots
            Where best;
            int64_t best_bad = INT64_MAX;
            int best_spot = -1;
            Stack best_stack = Stack::None;
            if (rule != nullptr) {
                for (int k = 0; k < 3; k++) {
                    const Spot& s = rule->spots[k];
                    if (k > 0 && s.x == X::Stack && s.stack == Stack::None) break;
                    Where w = place(e, s);
                    if (!w.ok) continue;
                    Rect box{ w.x + offset(w.origin), w.y, w.x + offset(w.origin) + size.w() - 1, w.y + size.h() - 1 };
                    int64_t bad = badness(e, box, s.skip);
                    if (bad < best_bad) {
                        best_bad = bad;
                        best = w;
                        best_spot = k;
                        best_stack = resolve(s.stack, e.player);
                    }
                    if (bad == 0) break;
                }
                e.rule = rule->name;
            }
            if (best_spot < 0) {
                // No rule (or none of its places works): it keeps its place (split: moved to its half or quadrant).
                best = place(e, at(X::Keep, 0, Y::Keep, 0));
                best_spot = 0;
                best_stack = Stack::None;
                best_bad = 0;
                if (rule == nullptr) e.rule = split ? "keeps its place in its half or quadrant" : "as the game lays it out";
                if (!split) {
                    best.x = e.game.x0;
                    best.y = e.game.y0;
                }
            }
            e.spot = best_spot;
            e.cramped = best_bad > 0 && rule != nullptr;
            e.x = best.x;
            e.y = best.y;
            e.origin = best.origin;
            e.box = Rect{ e.x + offset(e.origin), e.y, e.x + offset(e.origin) + size.w() - 1, e.y + size.h() - 1 };
            if (best_stack != Stack::None && e.player >= 0 && e.player < 4) {
                stacks[e.player][(int)best_stack].push_back(index);
            }
            placed.push_back(index);
        }

        void solve_split() {
            const auto& table = rules();
            // Rule order, then every element no rule placed (messages last).
            std::vector<bool> done(out.elements.size(), false);
            for (const Rule& rule : table) {
                for (size_t i = 0; i < out.elements.size(); i++) {
                    Element& e = out.elements[i];
                    if (done[i] || !e.possible || e.role != rule.role) continue;
                    uint32_t m = media(e);
                    if ((m & rule.when) != rule.when) continue;
                    place_element((int)i, &rule);
                    done[i] = true;
                }
            }
            for (int pass = 0; pass < 2; pass++) {
                for (size_t i = 0; i < out.elements.size(); i++) {
                    Element& e = out.elements[i];
                    if (done[i] || !e.possible || ((e.role == Role::Message) != (pass == 1))) continue;
                    place_element((int)i, nullptr);
                    done[i] = true;
                }
            }
        }

        // One view or two stacked: the game's layout. Each group of touching elements that can appear gets the origin
        // of the screen third its center is in; the odometer goes by its rule.
        void solve_single() {
            // Elements touch by their frames (where a moving part reaches doesn't join groups: the tachometer's
            // needle at full revs would take the gear from the right edge).
            std::vector<Rect> frames(out.elements.size());
            for (size_t i = 0; i < out.elements.size(); i++) {
                for (int k : out.elements[i].parts) {
                    if (out.parts[k].frame) frames[i].add(out.parts[k].opaque);
                }
                if (!frames[i].valid()) frames[i] = out.elements[i].game;
            }
            std::vector<int> parent(out.elements.size());
            for (size_t i = 0; i < parent.size(); i++) parent[i] = (int)i;
            for (size_t i = 0; i < out.elements.size(); i++) {
                const Element& a = out.elements[i];
                if (!a.possible || a.role == Role::Odometer) continue;
                for (size_t j = 0; j < i; j++) {
                    const Element& b = out.elements[j];
                    if (!b.possible || b.role == Role::Odometer) continue;
                    // Messages and banners come and go over the rest: they keep to themselves. A player's elements
                    // group with that player's only (the shared track map touches player 2's tachometer when it's at
                    // full revs, which would pull the speedometer to the right edge).
                    bool loose_a = a.role == Role::Message || a.role == Role::Banner;
                    bool loose_b = b.role == Role::Message || b.role == Role::Banner;
                    // The gear goes with its tachometer (5 px apart), so it stays where the game has it beside it at
                    // every aspect ratio.
                    bool gear_tach = (a.role == Role::Gear && b.role == Role::Tach) || (a.role == Role::Tach && b.role == Role::Gear);
                    if (loose_a != loose_b || (loose_a && a.group != b.group) || a.player != b.player ||
                        !(touching(frames[i], frames[j]) || gear_tach)) {
                        continue;
                    }
                    parent[find_root(parent, (int)i)] = find_root(parent, (int)j);
                }
            }
            // A group's bounds where it starts and, for one that slides (WRONG WAY), where the slide ends: it's
            // anchored by the third it's in at each end and slides from one anchor to the other.
            std::vector<Rect> bounds(out.elements.size()), ends(out.elements.size());
            for (size_t i = 0; i < out.elements.size(); i++) {
                const Element& e = out.elements[i];
                if (!e.possible) continue;
                int root = find_root(parent, (int)i);
                bounds[root].add(e.slides ? e.start : e.game);
                ends[root].add(e.slides ? e.finish : e.game);
            }
            for (size_t i = 0; i < out.elements.size(); i++) {
                Element& e = out.elements[i];
                if (e.role == Role::Odometer) continue;
                int root = find_root(parent, (int)i);
                const Rect& g = bounds[root];
                e.scale = 1.0f;
                e.x = e.game.x0;
                e.y = e.game.y0;
                e.origin = g.valid() ? origin_for_x((g.x0 + g.x1) / 2) : origin_none;
                e.origin_end = e.slides && ends[root].valid() ? origin_for_x((ends[root].x0 + ends[root].x1) / 2) : e.origin;
                e.box = e.game.moved(offset(e.origin), 0);
                if (e.slides) {
                    e.box = e.start.moved(offset(e.origin), 0);
                    e.box.add(e.finish.moved(offset(e.origin_end), 0));
                }
                e.spot = 0;
                e.rule = "as the game lays it out";
                // A hand placement (the HUD editor's) moves or scales it.
                e.scale = hand_scale(e, 1.0f);
                e.by_hand = e.scale != 1.0f;
                Rect size = scaled(e.game, e.scale);
                if (!place_by_hand(e, size) && e.by_hand) {
                    e.box = Rect{ e.box.x0, e.box.y0, e.box.x0 + size.w() - 1, e.box.y0 + size.h() - 1 };
                }
                if (e.possible) placed.push_back((int)i);
            }
            // The gear keeps its place beside its tachometer, and its size, when the tachometer is scaled or placed by
            // hand (and it isn't placed by hand itself). Player 2's (two stacked) goes where player 1's is beside it:
            // the ROM table's place for it (56 below its tachometer) isn't where the game draws it.
            for (Element& g : out.elements) {
                if (g.role != Role::Gear || !g.possible) continue;
                int t = by_role(g.player, Role::Tach);
                if (t < 0 || !out.elements[t].possible) continue;
                const Element& tach = out.elements[t];
                const Override* o = override_of(g);
                bool beside = g.player > 0;
                if ((o != nullptr && (o->flags & Override::moved)) || (!tach.by_hand && tach.scale == 1.0f && !beside)) continue;
                int32_t ox = beside ? follow_offset(g, tach, true) : g.game.x0 - tach.game.x0;
                int32_t oy = beside ? follow_offset(g, tach, false) : g.game.y0 - tach.game.y0;
                g.scale = hand_scale(g, tach.scale);
                g.origin = tach.origin;
                g.x = tach.x + (int32_t)std::lround(ox * tach.scale);
                g.y = tach.y + (int32_t)std::lround(oy * tach.scale);
                Rect size = scaled(g.game, g.scale);
                g.box = Rect{ g.x + offset(g.origin), g.y, g.x + offset(g.origin) + size.w() - 1, g.y + size.h() - 1 };
                g.by_hand = true;
                g.rule = "beside the tachometer as the game has it, at its size";
            }
            // Two stacked views: one stunt box per view as in split screen (the trick name in the scores' place, shown
            // instead of them while a trick is up: stunt_swap), at 80%, 2 px right of the radar with its bottom on the
            // radar's (the stunt track has no radar: in its place). Not one moved by hand.
            if (c.players == 2) {
                constexpr float stunt_stacked_scale = 0.8f;
                for (int p = 0; p < 2; p++) {
                    int r = by_role(p, Role::Radar);
                    if (r < 0) continue;
                    const Element& radar = out.elements[r];
                    bool shown = radar.possible && radar.box.valid();
                    Rect at = shown ? radar.box : radar.game.moved(offset(origin_left), 0);
                    int32_t x0 = shown ? at.x1 + 1 + 2 : at.x0;
                    for (Role role : { Role::StuntScore, Role::StuntTrick }) {
                        int i = by_role(p, role);
                        if (i < 0 || !out.elements[i].possible) continue;
                        Element& e = out.elements[i];
                        const Override* o = override_of(e);
                        if (o != nullptr && (o->flags & Override::moved)) continue;
                        e.scale = hand_scale(e, stunt_stacked_scale);
                        Rect size = scaled(e.game, e.scale);
                        e.origin = e.origin_end = origin_left;
                        e.box = Rect{ x0, at.y1 - size.h() + 1, x0 + size.w() - 1, at.y1 };
                        e.x = e.box.x0 - offset(e.origin);
                        e.y = e.box.y0;
                        e.by_hand = true;
                        e.rule = role == Role::StuntScore ? "stunt scores: right of the radar, at 80% (two stacked)"
                                                          : "trick name: in the stunt scores' place (two stacked)";
                    }
                }
            }
            for (const Rule& rule : rules()) {
                if (rule.role != Role::Odometer || !(rule.when & m_single)) continue;
                for (size_t i = 0; i < out.elements.size(); i++) {
                    if (out.elements[i].role == Role::Odometer && out.elements[i].possible) place_element((int)i, &rule);
                }
            }
        }

        void write_outputs() {
            for (size_t ei = 0; ei < out.elements.size(); ei++) {
                const Element& e = out.elements[ei];
                if (e.hidden) {
                    for (int i : e.parts) out.places[i].hidden = true;
                    continue;
                }
                if (!e.possible || e.spot < 0) continue;
                int32_t dx = e.x - e.game.x0;
                int32_t dy = e.y - e.game.y0;
                for (int i : e.parts) {
                    Place& pl = out.places[i];
                    pl.placed = true;
                    pl.dx = split || e.by_hand ? dx : 0;
                    pl.dy = split || e.by_hand ? dy : 0;
                    pl.origin = e.origin;
                    pl.origin_end = e.slides && !split ? e.origin_end : e.origin;
                    if (e.scale != 1.0f) {
                        pl.scale = e.scale;
                        pl.pivot_x = e.game.x0 + dx;
                        pl.pivot_y = e.game.y0 + dy;
                    }
                }
            }
            // The odometer.
            for (const Element& e : out.elements) {
                if (e.role != Role::Odometer || e.player < 0 || e.player > 3 || e.spot < 0) continue;
                OdometerPlace& o = out.odometer[e.player];
                o.valid = true;
                o.x = e.x;
                o.y = e.y;
                o.origin = e.origin;
                o.scale = e.scale;
            }
            for (int p = 0; p < 4; p++) {
                int score = by_role(p, Role::StuntScore), trick = by_role(p, Role::StuntTrick);
                if (score < 0 || trick < 0) continue;
                const Element& a = out.elements[score];
                const Element& b = out.elements[trick];
                // Sharing a place, not just an edge (boxes snapped side by side by hand touch on one column).
                bool shared = std::min(a.box.x1, b.box.x1) - std::max(a.box.x0, b.box.x0) >= 1 &&
                              std::min(a.box.y1, b.box.y1) - std::max(a.box.y0, b.box.y0) >= 1;
                out.stunt_swap[p] = a.possible && b.possible && a.spot >= 0 && b.spot >= 0 && shared;
            }
            if (!split) return;
            // Each player's time and position as drawn.
            for (int p = 0; p < 4; p++) {
                int t = by_role(p, Role::Time), l = by_role(p, Role::LapTime), pos = by_role(p, Role::Position);
                if (t < 0 || pos < 0 || !out.elements[t].possible || !out.elements[pos].possible) continue;
                Rect time = out.elements[t].box;
                if (l >= 0 && out.elements[l].possible) time.add(out.elements[l].box);
                const Rect& place = out.elements[pos].box;
                out.panel_rows[p] = PanelRow{ true, time.x0, time.x1, time.y0, place.x0, place.x1, place.y0,
                                              std::min(time.y0, place.y0), std::max(time.y1, place.y1) };
            }
            out.map = by_role(-1, Role::Map);
        }

        // ---- Rush 2049's battle HUD (src/rush2049/battle.cpp draws it where Output::battle says) ----
        // A view's part of the 4:3 screen (rush2::views::view_of).
        struct ViewRect {
            int x0, y0, x1, y1;
            bool right, bottom, wide;  // the right column or bottom row of a split; one of two stacked views
        };
        ViewRect view_rect(int p) const {
            if (std::clamp(c.players, 1, 4) <= 1) return { 0, 0, 320, 240, false, false, false };
            if (quads) {
                int x = (p & 1) * 160, y = (p >> 1) * 120;
                return { x, y, x + 160, y + 120, (p & 1) != 0, p >= 2, false };
            }
            if (c.arrangement == Arrangement::SideBySide) return { p * 160, 0, p * 160 + 160, 240, p == 1, false, false };
            return { 0, p * 120, 320, p * 120 + 120, false, p == 1, true };
        }

        // Where a view's battle HUD goes, as Rush 2049 lays it out (its tables 0x80394150, 0x803941D0, 0x803940D0,
        // 0x80394210): the health bar (HEALTHBG, 64 x 8) centered at the bottom; the weapon at the bottom on the view's
        // outer side; the coin at the corner toward the screen's middle (the right-hand views mirror the left-hand
        // ones: the weapon on the right). A race's HUD keeps the weapon left of the power-up in every view. With two players 2049 draws all of it a third
        // wider than with one, three or four.
        static constexpr float health_width = 64.0f, health_height = 8.0f;
        BattlePlace battle_arena_place(const ViewRect& v) const {
            BattlePlace l;
            l.valid = true;
            float w = (float)(v.x1 - v.x0), h = (float)(v.y1 - v.y0);
            bool tall = h > 120.0f;
            bool small = w <= 160.0f && h <= 120.0f;
            l.bar_sx = v.wide ? 4.0f / 3.0f : 1.0f;
            l.bar_sy = v.wide ? 8.0f / 7.0f : 1.0f;
            if (small) {
                // Quadrants: at small_scale, as the rest of the HUD there.
                l.bar_sx *= c.small_scale;
                l.bar_sy *= c.small_scale;
                l.weapon_scale = l.coin_scale = l.powerup_scale = c.small_scale;
            }
            // At the bottom of the view; with one player, higher (Rush 2049's).
            float bar_center_y = (float)v.y1 - (tall && w > 160.0f ? 32.0f : 13.0f);
            l.bar_x = (float)v.x0 + w * 0.5f - health_width * l.bar_sx * 0.5f;
            l.bar_y = bar_center_y - health_height * l.bar_sy * 0.5f;
            float side = v.wide ? 43.0f : w > 160.0f ? 32.0f : 40.0f;
            l.weapon_x = v.right ? (float)v.x1 - side : (float)v.x0 + side;
            l.weapon_y = bar_center_y;
            if (small) {
                // Quadrants: the coins meet at the middle of the screen, either side of Rush 2's clock there.
                l.coin_x = v.right ? (float)v.x0 + 34.0f : (float)v.x1 - 34.0f;
                l.coin_y = v.bottom ? (float)v.y0 + 12.0f : (float)v.y1 - 14.0f;
            }
            else {
                // Stacked views: toward the line between them, as 2049. Otherwise at the bottom right, on the bar's line.
                l.coin_x = (float)v.x1 - 23.0f;
                l.coin_y = v.wide ? (v.bottom ? (float)v.y0 + 14.0f : (float)v.y1 - 14.0f) : bar_center_y;
            }
            l.digits = small ? 10.0f * c.small_scale : 12.0f;  // the digits are 10 tall
            l.powerup_x = l.bar_x + health_width * l.bar_sx + 14.0f;
            l.powerup_y = bar_center_y;
            if (small) {
                // Quadrants: beside the bar it would meet the coin (toward the middle): over the bar's inner end, 2 px
                // clear of it.
                float ph = 20.0f * 0.7f * l.powerup_scale;
                l.powerup_x = v.right ? l.bar_x + ph * 0.5f : l.bar_x + health_width * l.bar_sx - ph * 0.5f;
                l.powerup_y = l.bar_y - 2.0f - ph * 0.5f;
            }
            else if (v.right && w <= 160.0f) l.powerup_x = l.bar_x - 14.0f;
            l.bar_anchor = (float)(v.x0 + v.x1) / 640.0f;
            l.weapon_anchor = (float)(v.right ? v.x1 : v.x0) / 320.0f;
            l.coin_anchor = (float)(l.coin_x > (float)(v.x0 + v.x1) * 0.5f ? v.x1 : v.x0) / 320.0f;
            l.ammo_x = l.weapon_x;
            return l;
        }

        // A race's bar (the BATTLE row and the Weapons cheat alike): centered at the bottom of the view; split, where
        // that runs into player p's time or position (the bottom row of quadrants in 4:3), above them.
        BattlePlace battle_race_bar(const ViewRect& v, int p) const {
            BattlePlace l = battle_arena_place(v);
            const PanelRow& row = out.panel_rows[p];
            if (!row.valid) return l;
            constexpr float gap = 2.0f;
            float bar_y1 = l.bar_y + health_height * l.bar_sy;
            if (bar_y1 < (float)row.y0 || l.bar_y > (float)row.y1 + 1.0f) return l;
            auto drawn = [&](float x, float anchor) { return x + (float)offset((uint16_t)std::lround(anchor * origin_right)); };
            float bar_x0 = drawn(l.bar_x, l.bar_anchor), bar_x1 = drawn(l.bar_x + health_width * l.bar_sx, l.bar_anchor);
            if (bar_x0 < (float)row.time_x1 + 1.0f + gap || bar_x1 > (float)row.place_x0 - gap) {
                // Between the time and the position if it fits there (2 px from them, else 1, else touching), as
                // near the middle as it can be.
                for (float g : { gap, 1.0f, 0.0f }) {
                    float lo = (float)row.time_x1 + 1.0f + g, hi = (float)row.place_x0 - g;
                    if (hi - lo < bar_x1 - bar_x0) continue;
                    float dx = std::clamp(bar_x0, lo, hi - (bar_x1 - bar_x0)) - bar_x0;
                    l.bar_x += dx;
                    l.powerup_x += dx;
                    return l;
                }
                // Else over them.
                float dy = (float)row.y0 - gap - bar_y1;
                l.bar_y += dy;
                l.powerup_y += dy;
            }
            return l;
        }

        // A race with the BATTLE row or the Weapons cheat (one layout: the bar never moves between them): the bar,
        // with the weapon held centered just over it (turning about its own middle), its ammo to its right (not with
        // the cheat: endless), and the power-up in effect over the bar's right end. Beside the bar they would run into
        // Rush 2's time, place and radar in split screen. The weapon is always left of the power-up, as in the battle
        // arenas.
        BattlePlace battle_race_place(const ViewRect& v, int p) const {
            constexpr float ammo_gap = 16.0f;  // from the weapon's middle to its ammo's
            constexpr float gap = 2.0f;
            BattlePlace l = battle_race_bar(v, p);
            bool small = v.x1 - v.x0 <= 160 && v.y1 - v.y0 <= 120;
            float k = small ? 0.7f : 1.0f;  // the models' boxes (place_battle's model())
            float wh = 24.0f * k * l.weapon_scale, ph = 20.0f * k * l.powerup_scale;
            l.weapon_anchor = l.bar_anchor;
            l.powerup_anchor = l.bar_anchor;
            float bw = health_width * l.bar_sx, bh = health_height * l.bar_sy;
            // The weapon held's reach from its middle: the model, and its ammo to the right (not with the cheat).
            float s = l.digits / 10.0f;
            float w_left = wh * 0.5f, w_right = c.battle_hud == 3 ? wh * 0.5f
                : std::max(wh * 0.5f, ammo_gap * l.bar_sx + 13.5f * s + s);
            auto put = [&](float wx, float wy, float px, float py, float dy) {
                l.weapon_x = wx;
                l.weapon_y = wy + dy;
                l.powerup_x = px;
                l.powerup_y = py + dy;
                l.bar_y += dy;
            };
            // Whether a box (4:3, the bar's anchor) is clear of the player's HUD and the track map (and, with one view,
            // Rush 2's own stunt boxes; split screen attaches the bar over them instead, two stacked pushes the row
            // right of them: done()), inside the view.
            auto drawn = [&](float x) { return x + (float)offset((uint16_t)std::lround(l.bar_anchor * origin_right)); };
            auto clear = [&](float x0, float y0, float x1, float y1) {
                Rect r{ (int32_t)std::floor(drawn(x0)), (int32_t)std::floor(y0), (int32_t)std::ceil(drawn(x1)) - 1, (int32_t)std::ceil(y1) - 1 };
                for (const Element& o : out.elements) {
                    if (!o.possible || o.spot < 0 || !o.box.valid() || battle_role(o.role)) continue;
                    if (o.player >= 0 && o.player != p) continue;
                    if (o.role == Role::Message || o.role == Role::Banner) continue;
                    if ((o.role == Role::StuntScore || o.role == Role::StuntTrick) && (split || v.wide)) continue;
                    if (o.box.overlaps(r)) return false;
                }
                // Inside the view with the HUD's margin (its edges on the screen's edges go out with the window's).
                return r.x0 >= v.x0 + margin + (v.x0 == 0 ? offset(origin_left) : 0) &&
                       r.x1 <= v.x1 - 1 - margin + (v.x1 == 320 ? offset(origin_right) : 0);
            };
            auto done = [&]() {
                if (v.wide) {
                    // Two stacked: the stunt box (right of the radar) pushes the row (power-up, bar, weapon and ammo)
                    // right until it's 2 px clear, so the row's order is the same with the stunt boxes on or off.
                    Rect stunt;
                    for (Role role : { Role::StuntScore, Role::StuntTrick }) {
                        const Element* s = ref(p, role);
                        if (s != nullptr && s->possible && s->spot >= 0 && s->box.valid()) stunt.add(s->box);
                    }
                    float x0 = drawn(std::min({ l.bar_x, l.weapon_x - w_left, l.powerup_x - ph * 0.5f }));
                    float x1 = drawn(std::max({ l.bar_x + bw, l.weapon_x + w_right, l.powerup_x + ph * 0.5f }));
                    float y0 = std::min({ l.bar_y, l.weapon_y - wh * 0.5f, l.powerup_y - ph * 0.5f });
                    float y1 = std::max({ l.bar_y + bh, l.weapon_y + wh * 0.5f, l.powerup_y + ph * 0.5f });
                    if (stunt.valid() && x0 <= (float)stunt.x1 + gap && x1 >= (float)stunt.x0 && y0 <= (float)stunt.y1 &&
                        y1 >= (float)stunt.y0) {
                        float dx = (float)stunt.x1 + 1.0f + gap - x0;
                        l.bar_x += dx;
                        l.weapon_x += dx;
                        l.powerup_x += dx;
                    }
                }
                l.ammo_x = l.weapon_x + ammo_gap * l.bar_sx;
                return l;
            };
            if (c.hud_width > 320.5f || c.arrangement == Arrangement::SideBySide) {
                // Widescreen (two side by side always: their views are tall enough for nothing over the bar): beside
                // the bar, bottoms lined up with its bottom (a bar attached over the stunt boxes keeps them clear), the
                // weapon left and the power-up right; else both lifted over the race time and position; the first
                // that's clear.
                const PanelRow& row = out.panel_rows[p];
                float by1 = l.bar_y + bh;
                float lift = row.valid && (float)row.y0 < by1 && (float)row.y1 + 1.0f > l.bar_y - std::max(wh, ph)
                    ? (float)row.y0 - gap - by1 : 0.0f;
                for (float dy : { 0.0f, lift }) {
                    if (dy > 0.0f) continue;  // only ever up
                    float wy = by1 - wh * 0.5f, py = by1 - ph * 0.5f;
                    float wx = l.bar_x - gap - w_right;
                    float px = l.bar_x + bw + gap + ph * 0.5f;
                    float top = std::min({ l.bar_y, wy - wh * 0.5f, py - ph * 0.5f }) + dy;
                    float x0 = std::min({ l.bar_x, wx - w_left, px - ph * 0.5f });
                    float x1 = std::max({ l.bar_x + bw, wx + w_right, px + ph * 0.5f });
                    // Where it is, else (on the time and position's row) slid sideways into the gap between them, as
                    // near the middle as it can be.
                    float shifts[2] = { 0.0f, 0.0f };
                    int n = 1;
                    if (dy == 0.0f && row.valid) {
                        float lo = (float)row.time_x1 + 1.0f + gap, hi = (float)row.place_x0 - gap;
                        float d0 = drawn(x0), w = x1 - x0;
                        if (hi - lo >= w) shifts[n++] = std::clamp(d0, lo, hi - w) - d0;
                    }
                    for (int k = 0; k < n; k++) {
                        float dx = shifts[k];
                        if (k > 0 && dx == 0.0f) continue;
                        if (clear(x0 + dx, top, x1 + dx, by1 + dy)) {
                            l.bar_x += dx;
                            put(wx + dx, wy, px + dx, py, dy);
                            return done();
                        }
                    }
                    if (lift == 0.0f) break;
                }
                // No room with the bar centered: the weapon, bar and power-up centered together on the view.
                {
                    float wy = by1 - wh * 0.5f, py = by1 - ph * 0.5f;
                    float row_w = w_left + w_right + gap + bw + gap + ph;
                    float x0 = (float)(v.x0 + v.x1) * 0.5f - row_w * 0.5f;
                    float dx = x0 + w_left + w_right + gap - l.bar_x;
                    float wx = x0 + w_left, px = l.bar_x + dx + bw + gap + ph * 0.5f;
                    float top = std::min({ l.bar_y, wy - wh * 0.5f, py - ph * 0.5f });
                    if (clear(x0, top, x0 + row_w, by1)) {
                        l.bar_x += dx;
                        put(wx, wy, px, py, 0.0f);
                        return done();
                    }
                }
            }
            // Stacked (4:3, or no room beside): the weapon held 2 px over the bar, centered on it with its ammo, else
            // the model alone centered; the power-up 2 px right of its ammo (the weapon left of it, as beside the bar);
            // the first that's clear, else slid sideways the least that's clear (up to half the bar), else the first.
            float sy = l.bar_y - gap - wh * 0.5f, mid = l.bar_x + bw * 0.5f;
            float top = sy - std::max(wh, ph) * 0.5f;
            int reach = (int)(bw * 0.5f);
            for (int d = 0; d <= reach; d++) {
                for (int sign : { 1, -1 }) {
                    if (d == 0 && sign < 0) continue;
                    for (int k = 0; k < 2; k++) {
                        float sx = (k == 1 ? mid : mid - (w_right - w_left) * 0.5f) + (float)(sign * d);
                        float px = sx + w_right + gap + ph * 0.5f;
                        if (clear(sx - w_left, top, px + ph * 0.5f, sy + wh * 0.5f)) {
                            put(sx, sy, px, sy, 0.0f);
                            return done();
                        }
                    }
                }
            }
            float sx = mid - (w_right - w_left) * 0.5f;
            put(sx, sy, sx + w_right + gap + ph * 0.5f, sy, 0.0f);
            return done();
        }

        // An element of the battle HUD at its place: (x0, y0) - (x1, y1) exclusive, 4:3, drawn with `anchor`.
        void add_battle_element(Role role, int p, float x0, float y0, float x1, float y1, float anchor, const char* rule,
                                float scale) {
            int i = add_element(role, p, 0);
            Element& e = out.elements[i];
            e.possible = true;
            e.scale = scale;
            e.game = Rect{ (int32_t)std::floor(x0), (int32_t)std::floor(y0), (int32_t)std::ceil(x1) - 1, (int32_t)std::ceil(y1) - 1 };
            e.origin = (uint16_t)std::lround(std::clamp(anchor, 0.0f, 1.0f) * origin_right);
            e.x = e.game.x0;
            e.y = e.game.y0;
            e.box = e.game.moved(offset(e.origin), 0);
            e.spot = 0;
            e.rule = rule;
            placed.push_back(i);
        }

        // The ammo or kills (up to 3 digits, rush2::hud::draw_number: 8 x 10 glyphs a texel apart, a shadow a texel
        // down and right) centered on (x, y), widening r (x0, y0, x1, y1).
        static void add_digits(float x, float y, float height, float r[4]) {
            float s = height / 10.0f, w = 9.0f * s * 3.0f;
            r[0] = std::min(r[0], x - w * 0.5f);
            r[1] = std::min(r[1], y - height * 0.5f);
            r[2] = std::max(r[2], x + w * 0.5f + s);
            r[3] = std::max(r[3], y + height * 0.5f + s);
        }

        // A battle HUD element moved by (dx, dy), its places in `l` with it.
        void move_battle(Element& e, BattlePlace& l, int32_t dx, int32_t dy) {
            switch (e.role) {
                case Role::BattleBar: l.bar_x += (float)dx; l.bar_y += (float)dy; break;
                case Role::BattleWeapon: l.weapon_x += (float)dx; l.ammo_x += (float)dx; l.weapon_y += (float)dy; break;
                case Role::BattleCoin: l.coin_x += (float)dx; l.coin_y += (float)dy; break;
                default: l.powerup_x += (float)dx; l.powerup_y += (float)dy; break;
            }
            e.game = e.game.moved(dx, dy);
            e.box = e.box.moved(dx, dy);
            e.x += dx;
            e.y += dy;
        }

        // The battle HUD's attachments (after its hand placements): an attached element moves to its offset from what
        // it's attached to. In a race the weapon held and the power-up in effect sit over the bar by rule, so they go
        // with an attached bar (unless placed by hand themselves).
        void battle_attach(int p, BattlePlace& l) {
            for (int pass = 0; pass < 3; pass++) {
                for (Element& e : out.elements) {
                    if (!battle_role(e.role) || e.player != p || !e.possible) continue;
                    const Override* o = override_of(e);
                    if (o == nullptr || !(o->flags & Override::attached)) continue;
                    const Element* to = attach_target(*o, e);
                    if (to == nullptr) continue;
                    int32_t dx = (o->flags & Override::attached_y) ? 0 : to->box.x0 + o->x - e.box.x0;
                    int32_t dy = (o->flags & Override::attached_x) ? 0 : to->box.y0 + o->y - e.box.y0;
                    e.by_hand = true;
                    e.rule = "attached by hand (HUD editor)";
                    if (dx == 0 && dy == 0) continue;
                    move_battle(e, l, dx, dy);
                    if (e.role != Role::BattleBar || c.battle_hud == 1) continue;
                    for (Element& f : out.elements) {
                        if (f.player != p || !f.possible || (f.role != Role::BattleWeapon && f.role != Role::BattlePowerup)) continue;
                        const Override* fo = override_of(f);
                        if (fo != nullptr && (fo->flags & (Override::moved | Override::attached))) continue;
                        move_battle(f, l, dx, dy);
                    }
                }
            }
        }

        // An arena's coin and kills where Rush 2049 has them run into the player's speedometer, tachometer or gear (the
        // bottom row of quadrants in 4:3): 2 px under those.
        void coin_clear(int p, BattlePlace& l) const {
            bool small = l.digits < 12.0f;
            float size = 24.0f * (small ? 0.7f : 1.0f) * l.coin_scale;
            float r[4] = { l.coin_x - size * 0.5f, l.coin_y - size * 0.5f, l.coin_x + size * 0.5f, l.coin_y + size * 0.5f };
            add_digits(l.coin_x, l.coin_y, l.digits, r);
            int32_t dx = offset((uint16_t)std::lround(std::clamp(l.coin_anchor, 0.0f, 1.0f) * origin_right));
            Rect box{ (int32_t)std::floor(r[0]) + dx, (int32_t)std::floor(r[1]), (int32_t)std::ceil(r[2]) - 1 + dx, (int32_t)std::ceil(r[3]) - 1 };
            int32_t below = INT32_MIN;
            for (Role role : { Role::Speed, Role::Tach, Role::Gear }) {
                const Element* e = ref(p, role);
                if (e != nullptr && e->box.overlaps(box)) below = std::max(below, e->box.y1);
            }
            if (below == INT32_MIN) return;
            l.coin_y += (float)(below + 1 + 2) - r[1];
        }

        void place_battle() {
            if (c.battle_hud < 1 || c.battle_hud > 3) return;
            for (int p = 0; p < std::clamp(c.players, 1, 4); p++) {
                ViewRect v = view_rect(p);
                BattlePlace l = c.battle_hud == 1 ? battle_arena_place(v) : battle_race_place(v, p);
                if (c.battle_hud == 1) coin_clear(p, l);
                l.powerup_anchor = l.bar_anchor;
                bool small = l.digits < 12.0f;
                float q = small ? c.small_scale : 1.0f;
                add_battle_element(Role::BattleBar, p, l.bar_x, l.bar_y, l.bar_x + health_width * l.bar_sx,
                                   l.bar_y + health_height * l.bar_sy, l.bar_anchor, "battle: health bar, Rush 2049's place", q);
                // The models' sizes on the screen aren't known outside the game: about 24 pixels (the coin: a tenth of
                // the screen's height), less in a quadrant [I].
                auto model = [&](float x, float y, float size, float r[4]) {
                    if (small) size *= 0.7f;
                    r[0] = x - size * 0.5f;
                    r[1] = y - size * 0.5f;
                    r[2] = x + size * 0.5f;
                    r[3] = y + size * 0.5f;
                };
                float r[4];
                {
                    model(l.weapon_x, l.weapon_y, 24.0f * l.weapon_scale, r);
                    if (c.battle_hud != 3) add_digits(l.ammo_x, l.weapon_y, l.digits, r);  // the cheat's are endless
                    add_battle_element(Role::BattleWeapon, p, r[0], r[1], r[2], r[3], l.weapon_anchor,
                                       "battle: weapon held and its ammo, Rush 2049's place", q);
                    model(l.powerup_x, l.powerup_y, 20.0f * l.powerup_scale, r);
                    add_battle_element(Role::BattlePowerup, p, r[0], r[1], r[2], r[3], l.bar_anchor,
                                       "battle: power-up in effect, Rush 2049's place", q);
                }
                if (c.battle_hud == 1) {
                    model(l.coin_x, l.coin_y, 24.0f * l.coin_scale, r);
                    add_digits(l.coin_x, l.coin_y, l.digits, r);
                    add_battle_element(Role::BattleCoin, p, r[0], r[1], r[2], r[3], l.coin_anchor,
                                       "battle: coin and kills, Rush 2049's place", q);
                }
                battle_by_hand(p, l);
                battle_attach(p, l);
                out.battle[p] = l;
            }
        }

        // The battle HUD's hand placements: each element's places move with it to where it was put (its anchor the
        // placement's), scale (the bar's frame, the models) or hide.
        void battle_by_hand(int p, BattlePlace& l) {
            for (Element& e : out.elements) {
                if (!battle_role(e.role) || e.player != p) continue;
                const Override* o = override_of(e);
                if (o == nullptr) continue;
                if (o->flags & Override::hidden) {
                    e.hidden = true;
                    e.possible = false;
                    (e.role == Role::BattleBar ? l.show_bar : e.role == Role::BattleWeapon ? l.show_weapon
                        : e.role == Role::BattleCoin ? l.show_coin : l.show_powerup) = false;
                    continue;
                }
                float s = (o->flags & Override::scaled) ? o->scale : 1.0f;
                float dx = 0, dy = 0;
                if (o->flags & Override::moved) {
                    dx = (float)(o->x - e.game.x0);
                    dy = (float)(o->y - e.game.y0);
                }
                float anchor = (o->flags & Override::moved)
                    ? (o->origin >= origin_none ? 0.5f : (float)o->origin / origin_right) : -1.0f;
                switch (e.role) {
                    case Role::BattleBar:
                        l.bar_x += dx;
                        l.bar_y += dy;
                        l.bar_sx *= s;
                        l.bar_sy *= s;
                        if (anchor >= 0) l.bar_anchor = anchor;
                        break;
                    case Role::BattleWeapon:
                        l.weapon_x += dx;
                        l.ammo_x += dx;
                        l.weapon_y += dy;
                        l.weapon_scale *= s;
                        if (anchor >= 0) l.weapon_anchor = anchor;
                        break;
                    case Role::BattleCoin:
                        l.coin_x += dx;
                        l.coin_y += dy;
                        l.coin_scale *= s;
                        if (anchor >= 0) l.coin_anchor = anchor;
                        break;
                    default:
                        l.powerup_x += dx;
                        l.powerup_y += dy;
                        l.powerup_scale *= s;
                        if (anchor >= 0) l.powerup_anchor = anchor;
                        break;
                }
                e.game = scaled(e.game.moved((int32_t)dx, (int32_t)dy), s);
                if (anchor >= 0) e.origin = (uint16_t)std::lround(anchor * origin_right);
                e.x = e.game.x0;
                e.y = e.game.y0;
                e.scale *= s;
                e.box = e.game.moved(offset(e.origin), 0);
                e.by_hand = true;
                e.rule = "placed by hand (HUD editor)";
            }
        }

        // Hand attachments (Override::attached): an element keeps its offset from another's top left corner wherever
        // that one went, taking its anchor. Chains are followed (a few passes); a missing or hidden target leaves it
        // where its rule put it.
        // What an attached element is attached to, if it's shown.
        const Element* attach_target(const Override& o, const Element& e) const {
            for (const Element& x : out.elements) {
                if (x.role != o.to_role || x.player != o.to_player || x.group != o.to_group || &x == &e) continue;
                if (!x.possible || x.hidden || x.spot < 0 || !x.box.valid()) return nullptr;
                return &x;
            }
            return nullptr;
        }

        void attach() {
            for (int pass = 0; pass < 4; pass++) {
                for (Element& e : out.elements) {
                    if (!e.possible || battle_role(e.role)) continue;
                    const Override* o = override_of(e);
                    if (o == nullptr || !(o->flags & Override::attached)) continue;
                    const Element* to = attach_target(*o, e);
                    if (to == nullptr) continue;
                    Rect size = e.box.moved(-e.box.x0, -e.box.y0);
                    int32_t x0 = (o->flags & Override::attached_y) ? e.box.x0 : to->box.x0 + o->x;
                    int32_t y0 = (o->flags & Override::attached_x) ? e.box.y0 : to->box.y0 + o->y;
                    if (!(o->flags & Override::attached_y)) {
                        e.origin = to->origin;  // it moves with that one sideways: its anchor too
                        e.origin_end = to->origin;
                    }
                    e.box = size.moved(x0, y0);
                    e.x = e.box.x0 - offset(e.origin);
                    e.y = e.box.y0;
                    e.spot = 0;
                    e.cramped = false;
                    e.by_hand = true;
                    e.rule = "attached by hand (HUD editor)";
                }
            }
        }

        void run() {
            out.split = split;
            out.places.assign(out.parts.size(), Place{});
            build_elements();
            if (split) solve_split();
            else solve_single();
            attach();
            write_outputs();
            place_battle();
        }
    };
}

namespace rush2::hud_layout {
    Output solve(const Config& config) {
        Output out;
        out.config = config;
        out.parts = rush2::hud_catalog::parts(variant_of(config));
        Solver s(out.config, out);
        s.run();
        return out;
    }

    TextPlace place_text(const Output& out, int32_t x, int32_t y, const std::vector<bool>* shown) {
        TextPlace t;
        bool quads = out.config.arrangement == Arrangement::Quadrants;
        if (out.split) {
            move_to_side(Rect{ x, y, x, y }, quads, -1, t.dx, t.dy);
            t.origin = side_origin_for_x(x + t.dx);
        }
        else {
            t.origin = origin_for_x(x);
        }
        int64_t best = INT64_MAX;
        for (size_t i = 0; i < out.parts.size(); i++) {
            const Part& p = out.parts[i];
            const Place& pl = out.places[i];
            if (!(pl.placed || pl.hidden) || !p.present || !p.reach.contains(x, y, text_margin)) continue;
            if (shown != nullptr && i < shown->size() && !(*shown)[i]) continue;
            int64_t a = area_of(p.reach);
            if (a < best) {
                best = a;
                t.dx = pl.dx;
                t.dy = pl.dy;
                t.origin = pl.origin;
                t.element = pl.element;
                t.hidden = pl.hidden;
            }
        }
        return t;
    }

    std::vector<Issue> check(const Output& out) {
        std::vector<Issue> issues;
        auto name = [&](const Element& e) {
            char b[64];
            if (e.player >= 0) std::snprintf(b, sizeof(b), "P%d %s", e.player + 1, role_name(e.role));
            else std::snprintf(b, sizeof(b), "%s", role_name(e.role));
            return std::string(b);
        };
        auto add = [&](bool error, const std::string& s) { issues.push_back(Issue{ error, s }); };
        const Config& c = out.config;
        bool quads = c.arrangement == Arrangement::Quadrants;
        float spread = c.hud_width - (float)screen_width;
        int32_t screen_x0 = (int32_t)std::lround(-0.5f * spread), screen_x1 = screen_width - 1 + (int32_t)std::lround(0.5f * spread);
        // Two parts the game could build alike: their widgets can't be told apart, so one would be placed as the other.
        for (size_t i = 0; i < out.parts.size(); i++) {
            const Part& p = out.parts[i];
            if (p.id.kind == rush2::hud_catalog::Identity::Kind::None) continue;
            int first = rush2::hud_catalog::find(out.parts, p.id);
            if (first >= 0 && first != (int)i) {
                const Part& q = out.parts[first];
                char b[160];
                std::snprintf(b, sizeof(b), "catalog: parts %d (%s P%d) and %zu (%s P%d) have the same build identity", first,
                              role_name(q.role), q.player + 1, i, role_name(p.role), p.player + 1);
                add(true, b);
            }
        }
        for (size_t i = 0; i < out.elements.size(); i++) {
            const Element& a = out.elements[i];
            if (!a.possible || a.spot < 0) continue;
            char b[256];
            if (a.cramped) {
                std::snprintf(b, sizeof(b), "%s: no place by its rule (%s) is clear", name(a).c_str(), a.rule);
                add(true, b);
            }
            if (a.box.x0 < screen_x0 || a.box.x1 > screen_x1 || a.box.y0 < 0 || a.box.y1 >= screen_height) {
                std::snprintf(b, sizeof(b), "%s is off the screen at (%d,%d)-(%d,%d)", name(a).c_str(), a.box.x0, a.box.y0, a.box.x1, a.box.y1);
                add(a.role != Role::Message, b);
            }
            if (out.split && a.player >= 0) {
                Area ar = split_edges(a.player, quads);
                bool right = (a.player & 1) != 0;
                Rect lim{ right ? ar.x0 : screen_x0, ar.y0, right ? screen_x1 : ar.x1, ar.y1 };
                if (a.box.x0 < lim.x0 || a.box.x1 > lim.x1 || a.box.y0 < lim.y0 || a.box.y1 > lim.y1) {
                    std::snprintf(b, sizeof(b), "%s reaches out of the player's view", name(a).c_str());
                    add(a.role != Role::Message, b);
                }
            }
            for (size_t j = 0; j < i; j++) {
                const Element& o = out.elements[j];
                if (!o.possible || o.spot < 0 || allowed_overlap(a.role, o.role)) continue;
                if (a.player >= 0 && o.player >= 0 && a.player != o.player && !out.split) continue;
                if (a.box.overlaps(o.box)) {
                    // One view or two stacked is the game's own layout: noted, not an error.
                    std::snprintf(b, sizeof(b), "%s overlaps %s", name(a).c_str(), name(o).c_str());
                    add(out.split, b);
                }
            }
        }
        return issues;
    }
}
