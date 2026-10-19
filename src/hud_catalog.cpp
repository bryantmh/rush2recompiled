// The race HUD's catalog (include/hud_catalog.h, docs/hud_layout.md#catalog): every part the race HUD draws, where
// the game draws it and where it can move, by layout.
//
// The race HUD setup (hud_race_setup_widgets_800A06F8) builds one element per entry of its layout table (0x800C2DB4,
// 112 entries: image name, x, y, callback, parameter; src/hud_catalog_rom.inc has them, read from the ROM) and the
// stunt panel's and bonus time's boxes (func_80093B38); the finish overlay (func_800A0FB4) builds its own table's
// (0x800BF328) and the place boxes. The entries' positions are where the game puts them with two views stacked;
// each entry's callback then hides it (another player's in a one player race, a setting turned off), may move it (the
// radar with one view, the needle on its arc) and picks the cell of its image it shows (the digits). Those callbacks
// are read here as rules: each part's `at` is where the game draws it for the variant and `reach` everywhere it can
// be drawn, with `note` naming the code. Players 3 and 4's parts are copies of player 2's (src/players4.cpp) at
// player 2's place. The layout engine (src/hud_layout.cpp) moves whole elements from there.
#include "hud_catalog.h"

#include <cstdio>
#include <cstring>

namespace rush2::hud_catalog {
    namespace {
        struct RomEntry {
            const char* name;
            int16_t x, y;
            uint32_t callback, param;
        };
#include "hud_catalog_rom.inc"

        constexpr int screen_h = 240;

        Rect rect(int x, int y, int w, int h) {
            return Rect{ x, y, x + w - 1, y + h - 1 };
        }
        Rect cat(const Rect& a, const Rect& b) {
            Rect r = a;
            r.add(b);
            return r;
        }

        // The player of an element (callback, parameter): in the parameter's low nibble or its second byte, by
        // callback (src/players4.cpp element_player); -1 for shared ones.
        int entry_player(uint32_t callback, uint32_t param) {
            switch (callback) {
                case 0x800B7B1C: case 0x800B7E88: case 0x800B9C44: case 0x800B9CC0: case 0x800B920C:
                    return (int)(param & 0xF);
                case 0x800BA2A8: case 0x800B9754: case 0x800B9978: case 0x800BA608: case 0x800B92F4:
                case 0x800BA868: case 0x800B8CC8: case 0x800B5A90:
                    return (int)((param >> 8) & 0xF);
                default:
                    return -1;
            }
        }
        uint32_t with_player(uint32_t callback, uint32_t param, int player) {
            switch (callback) {
                case 0x800B7B1C: case 0x800B7E88: case 0x800B9C44: case 0x800B9CC0: case 0x800B920C:
                    return (param & ~0xFu) | (uint32_t)player;
                default:
                    return (param & ~0xF00u) | ((uint32_t)player << 8);
            }
        }
        // The callbacks whose player 2 entries src/players4.cpp copies for players 3 and 4 (cloned_callbacks).
        bool cloned(uint32_t callback) {
            switch (callback) {
                case 0x800B7B1C: case 0x800B7E88: case 0x800BA2A8: case 0x800B9754: case 0x800B9978: case 0x800BA608:
                case 0x800B92F4: case 0x800BA868: case 0x800B9C44: case 0x800B9CC0: case 0x800B920C: case 0x800B8CC8:
                case 0x800B5A90:
                    return true;
                default:
                    return false;
            }
        }

        Identity entry_identity(const RomEntry& e, uint32_t param) {
            Identity id;
            id.kind = Identity::Kind::Entry;
            std::snprintf(id.name, sizeof(id.name), "%s", e.name);
            id.x = e.x;
            id.y = e.y;
            id.callback = e.callback;
            id.param = param;
            return id;
        }

        // An image part of entry e for player p (its parameter already the player's), at the entry's place.
        Part image_part(const RomEntry& e, uint32_t param, int player, const Variant& v) {
            Part p;
            p.kind = Kind::Image;
            p.player = (int8_t)player;
            p.id = entry_identity(e, param);
            p.texture = e.name;
            const Texture* t = texture(e.name, v.language);
            int w = t ? t->w : 0, h = t ? t->h : 0;
            p.at = rect(e.x, e.y, w, h);
            if (t) p.opaque = Rect{ e.x + t->opaque.x0, e.y + t->opaque.y0, e.x + t->opaque.x1, e.y + t->opaque.y1 };
            return p;
        }
        // Shows only cell `cell` (cw x ch, rows of cells left to right) of its image.
        void use_cell(Part& p, int cw, int ch, int cell) {
            p.cell_w = (int16_t)cw;
            p.cell_h = (int16_t)ch;
            p.cell = (int16_t)cell;
            p.at = rect(p.at.x0, p.at.y0, cw, ch);
            p.opaque = p.at;
        }
        void move_to(Part& p, int x, int y) {
            int dx = x - p.at.x0, dy = y - p.at.y0;
            p.at = p.at.moved(dx, dy);
            p.opaque = p.opaque.moved(dx, dy);
        }

        // The part(s) of one table entry for one player.
        void entry_parts(std::vector<Part>& out, const RomEntry& e, uint32_t param, int player, const Variant& v) {
            Part p = image_part(e, param, player, v);
            bool second = player > 0;     // players 2-4 are drawn at player 2's place, 110 lower
            uint32_t sub = param & 0xF;   // byte-player callbacks: which part of the element
            switch (e.callback) {
                case 0x800B7B1C:  // hud_widget_cb_tach_frame_800B7B1C: the tachometer's dim arc
                    p.role = Role::Tach;
                    p.frame = true;
                    p.note = "tachometer: TACH (800B7B1C), shown in manual";
                    break;
                case 0x800B7E88:  // hud_widget_cb_tach_needle_800B7E88: its lit part, or the needle on the arc
                    p.role = Role::Tach;
                    if ((param >> 4) & 0xF) {
                        p.note = "tachometer: its lit part (800B7E88), a part of TACH from its left edge";
                    }
                    else {
                        // x = 157 + 63 cos(a), clamped to 112-204; y = 1 - 41 sin(a), at least 30, +110 for every
                        // car but the first (a from 215 to 315 degrees with the revs).
                        // Its reach: where its opaque texels can be (not its image's transparent edges).
                        int dy = second ? 110 : 0;
                        const Texture* t = texture(e.name, v.language);
                        int w = t ? t->w : 16, h = t ? t->h : 7;
                        p.at = rect(112, 30 + dy, w, h);
                        Rect o = t ? t->opaque : Rect{ 0, 0, w - 1, h - 1 };
                        p.opaque = Rect{ 112 + o.x0, 30 + dy + o.y0, 112 + o.x1, 30 + dy + o.y1 };
                        p.reach = Rect{ 112 + o.x0, 30 + dy + o.y0, 204 + o.x1, 42 + dy + o.y1 };
                        p.note = "tachometer: its needle (800B7E88), moving on the arc: x 112-204, y 30-42";
                    }
                    break;
                case 0x800BA2A8:  // hud_speedometer_800BA2A8
                    p.role = Role::Speed;
                    if (sub == 4) {
                        p.frame = true;
                        p.note = "speedometer: SPEEDBG (800BA2A8)";
                    }
                    else if (sub == 3) {
                        use_cell(p, 32, 7, 0);
                        p.note = "speedometer: MPH or KPH (800BA2A8)";
                    }
                    else {
                        use_cell(p, 16, 13, sub == 0 ? 0 : sub == 1 ? 9 : 5);
                        p.note = "speedometer: a digit (800BA2A8)";
                    }
                    break;
                case 0x800B9754:  // hud_widget_cb_race_time_800B9754: the race time
                case 0x800B9978:  // hud_widget_cb_lap_time_800B9978: the lap (checkpoint) time, under it
                    p.role = e.callback == 0x800B9754 ? Role::Time : Role::LapTime;
                    if (sub == 9) {
                        p.frame = true;
                        p.note = e.callback == 0x800B9754 ? "race time: TIMETOP (800B9754)"
                                                          : "lap time box: TIMETOP (800B9978), for a few seconds after a checkpoint";
                    }
                    else {
                        // 0:29.86: the digits' cells (7 x 11, 0-9 then ':' at 10 and '.' at 11).
                        static const int sample[8] = { 0, 2, 9, 8, 6, 0, 10, 11 };
                        use_cell(p, 7, 11, sample[sub & 7]);
                        p.note = "time: a digit (800B9754, 800B9978)";
                    }
                    break;
                case 0x800B94C0:  // hud_widget_cb_time_left_800B94C0: the time left (shared)
                    p.role = Role::TimeLeft;
                    p.player = -1;
                    if (sub == 3) {
                        p.frame = true;
                        p.note = "time left: COUNTDOWNBG (800B94C0)";
                    }
                    else {
                        use_cell(p, 16, 17, sub == 0 ? 0 : sub == 1 ? 2 : 6);
                        p.note = "time left: a digit (800B94C0)";
                    }
                    break;
                case 0x800BA608:  // hud_widget_cb_race_position_800BA608
                    p.role = Role::Position;
                    if (e.name[0] == 'C') {
                        p.frame = true;
                        p.note = "position: COUNTDOWNBG (800BA608)";
                    }
                    else {
                        // 7/8: DIGIT0's place digit, DIGIT1's '/' (10) and the count.
                        bool big = std::strcmp(e.name, "DIGIT0") == 0;
                        use_cell(p, 16, big ? 17 : 13, big ? 7 : sub == 0xA ? 10 : 8);
                        p.note = "position: a digit (800BA608)";
                    }
                    break;
                case 0x800B92F4:  // hud_widget_cb_gear_800B92F4: the gear box (manual)
                    p.role = Role::Gear;
                    if ((param >> 12) & 0xF) {
                        // The lit gear: the row of GEAR (6 tall) for the gear, at its place in the panel (whose opaque
                        // texels are where it can be).
                        p.reach = p.opaque;
                        use_cell(p, 32, 6, 1);
                        move_to(p, p.at.x0, p.at.y0 + 6);
                        p.note = "gear: the lit gear (800B92F4), a row of the panel";
                    }
                    else {
                        p.frame = true;
                        p.note = "gear: GEAR, dim (800B92F4), shown in manual";
                    }
                    break;
                case 0x800BA868: {  // hud_widget_cb_lap_banner_800BA868: WRONG WAY and its three arrows
                    p.role = Role::Message;
                    // At (88, 50) (player 2 and up: 160), the arrows under it at x 160 - 16 k, 16 lower, blinking. With
                    // one player it slides away from 3 s to 3.5 s after it shows, in a straight line, to (22, 180), the
                    // arrows to x 60 - 16 k (130 lower: the game's 142 is patched in us.toml so the arrows sit 12 above
                    // the bottom, as far as they are from the left, instead of on its last row); with more it hides at
                    // 3 s. One element: they move together.
                    int k = (int)sub;
                    int base_y = second ? 160 : 50;
                    int x = k == 0 ? 88 : 160 - 16 * k;
                    int y = base_y + (k == 0 ? 0 : 16);
                    move_to(p, x, y);
                    p.reach = p.at;
                    if (v.players < 2) {
                        int x1 = k == 0 ? 22 : 60 - 16 * k;
                        p.end = p.at.moved(x1 - x, 130);
                        p.reach.add(p.end);
                    }
                    p.group = message_wrong_way;
                    p.note = k == 0 ? "WRONG WAY (800BA868): slides to the bottom left at 3 s with one player"
                                    : "WRONG WAY's arrow (800BA868), slides with it";
                    break;
                }
                case 0x800B9C44:  // hud_widget_cb_deaths_skull_800B9C44: the DEATHS race option's skull
                    p.role = Role::Deaths;
                    p.frame = true;
                    // x 80, or 12 with the race time turned off.
                    move_to(p, v.time_shown ? 80 : 12, p.at.y0);
                    p.note = "deaths: CROSSBONES (800B9C44), with the DEATHS option; x 12 without the race time";
                    break;
                case 0x800B9CC0:  // hud_widget_cb_it_badge_800B9CC0: the IT badge
                    p.role = Role::It;
                    p.frame = e.name[0] == 'I' && e.name[2] == 'B';
                    p.note = "IT badge (800B9CC0): stunt mode with a mode option, on the player who is it";
                    break;
                case 0x80060F24:  // hud_widget_cb_bonus_time_banner_80060F24 (shared)
                    p.role = Role::Message;
                    p.player = -1;
                    if (v.players >= 2) move_to(p, p.at.x0, p.at.y0 - 50);
                    p.group = message_bonus;
                    p.note = "BONUS TIME (80060F24): 50 higher with two or more players";
                    break;
                case 0x800B920C:  // hud_widget_cb_radar_800B920C
                    p.role = Role::Radar;
                    p.frame = true;
                    if (v.views > 1) move_to(p, p.at.x0, second ? 162 : 52);
                    p.note = "radar: MAPFRAME (800B920C), at y 70 with one view, else 52 (player 2 and up: 162)";
                    break;
                case 0x800B8CC8: {  // hud_widget_cb_radar_dots_800B8CC8: the radar's car dots, inside it
                    p.role = Role::Radar;
                    int y = v.views > 1 ? (second ? 162 : 52) : 70;
                    Rect radar = rect(17, y, 32, 64);
                    use_cell(p, 2, 4, (int)sub);
                    move_to(p, 32, y + 14);
                    p.reach = radar;
                    p.note = "radar: a car's dot (800B8CC8), in the radar";
                    break;
                }
                case 0x800B8188:  // minimap_draw_800B8188 (shared): at (207, 72), the track drawn into its texels
                    p.role = Role::Map;
                    p.player = -1;
                    p.frame = true;
                    move_to(p, 207, 72);
                    p.opaque = p.at;  // the track is drawn into it at run time: its whole image
                    p.note = "track map: TRACKMAPBG4 (800B8188) at (207, 72), the track drawn in it";
                    break;
                case 0x800B8900:  // hud_widget_cb_map_dots_800B8900: a car's dot on the track map
                    p.role = Role::Map;
                    p.player = -1;
                    use_cell(p, 4, 4, (int)sub);
                    move_to(p, 253, 118);
                    p.reach = rect(207, 72, 96, 96);
                    p.note = "track map: a car's dot (800B8900), on the map";
                    break;
                case 0x800B9D48: {  // hud_sprite_cb_laps_left_800B9D48 (shared): the finish flag and laps left
                    p.role = Role::LapsLeft;
                    p.player = -1;
                    // The finish point (X, Y): X 211-299 (mirrored 210-298), Y 76-164, nudged by track (0x800C3F84,
                    // 0x800C3FB4: x -4 to 1, y -4 to 0). The flag (a 15 x 16 cell) ends at it (mirrored: starts),
                    // the laps-left number (font 1, up to 2 digits) is printed at (X - 6, Y - 8) (mirrored X + 2).
                    use_cell(p, 15, 16, 0);
                    move_to(p, 240, 104);
                    p.reach = Rect{ 191, 57, 319, 164 };
                    p.note = "laps left: the finish flag and number (800B9D48), at the finish on the track map";
                    break;
                }
                case 0x800B5A90: {  // hud_widget_cb_finish_place_800B5A90: the finish overlay's place
                    p.role = Role::Banner;
                    // 1STPLACE, or a place (ENDPLACE's 48 x 24 cells) then "PLACE" at x 155; y 144 with one view.
                    int y = v.views == 1 ? 144 : p.at.y0;
                    if (sub == 0) {
                        use_cell(p, 48, 24, 1);
                        move_to(p, 96, y);
                    }
                    else if (sub == 1) {
                        move_to(p, 155, y);
                    }
                    else {
                        move_to(p, p.at.x0, y);
                    }
                    p.frame = sub != 1;
                    p.note = "finish: the player's place (800B5A90), once finished";
                    break;
                }
                case 0x800B5CAC:  // menu_widget_handler_delay_to_state_800B5CAC: GAME OVER (shared)
                    p.role = Role::Message;
                    p.player = -1;
                    if (v.views >= 2) move_to(p, p.at.x0, 110);
                    p.group = message_game_over;
                    p.note = "GAME OVER (800B5CAC): y 110 with two or more views";
                    break;
                case 0x800B7654:  // hud_widget_text_cb_player_stats_800B7654: prints the stunt panels' text
                    p.kind = Kind::Text;
                    p.role = Role::Other;
                    p.player = -1;
                    p.at = Rect{};
                    p.note = "stunt panel text (800B7654): printed in each player's boxes";
                    break;
                default:
                    p.note = "not cataloged";
                    break;
            }
            if (!p.reach.valid()) p.reach = p.at;
            out.push_back(p);
        }

        // A box (func_80093B38: x0, y0 to x1, y1 exclusive, a callback and parameter): its top edge, left edge, fill,
        // right edge and bottom edge.
        void box_parts(std::vector<Part>& out, int x0, int y0, int x1, int y1, uint32_t callback, uint32_t param,
                       Role role, int player, int dy, uint32_t fill, const char* note) {
            Rect parts[5] = {
                Rect{ x0, y0, x1 - 1, y0 }, Rect{ x0, y0 + 1, x0, y1 - 2 }, Rect{ x0 + 1, y0 + 1, x1 - 2, y1 - 2 },
                Rect{ x1 - 1, y0 + 1, x1 - 1, y1 - 2 }, Rect{ x0, y1 - 1, x1 - 1, y1 - 1 },
            };
            for (int i = 0; i < 5; i++) {
                Part p;
                p.kind = Kind::Fill;
                p.role = role;
                p.player = (int8_t)player;
                p.frame = true;
                p.at = parts[i].moved(0, dy);
                p.reach = p.at;
                p.opaque = p.at;
                p.id.kind = Identity::Kind::Box;
                p.id.x = (int16_t)x0;
                p.id.y = (int16_t)y0;
                p.id.x1 = (int16_t)x1;
                p.id.y1 = (int16_t)y1;
                p.id.callback = callback;
                p.id.param = param;
                p.id.part = (int8_t)i;
                p.color = i == 2 ? fill : 0x8C8C8CC0;
                p.note = note;
                out.push_back(p);
            }
        }

        void stunt_text(std::vector<Part>& out, int player, const Rect& score, const Rect& trick) {
            // hud_widget_text_cb_player_stats_800B7654: the trick name centered on x 74, BEST and TOTAL right-aligned
            // at x 258 (from x 215 and 202 in English) with their values from x 268, on rows 44 and 28 above the boxes'
            // bottom row's line (y 120 or 230).
            int base = score.y0 < screen_h / 2 ? 120 : 230;
            auto text = [&](Role role, const Rect& r, const char* s) {
                Part p;
                p.kind = Kind::Text;
                p.role = role;
                p.player = (int8_t)player;
                p.at = r;
                p.reach = r;
                p.opaque = r;
                p.text = s;
                p.color = 0xFFFFFFFF;
                p.note = "stunt panel text (800B7654)";
                out.push_back(p);
            };
            text(Role::StuntScore, Rect{ 202, base - 44, 296, base - 37 }, "BEST 1200");
            text(Role::StuntScore, Rect{ 202, base - 28, 296, base - 21 }, "TOTAL 4500");
            text(Role::StuntTrick, Rect{ trick.x0 + 8, base - 44, trick.x1 - 8, base - 37 }, "BACKFLIP X2");
        }
    }

    const char* role_name(Role role) {
        static const char* const names[] = { "Other", "Time", "Speed", "Position", "Radar", "Banner", "Deaths",
                                             "TimeLeft", "Map", "Tach", "Gear", "StuntScore", "StuntTrick", "LapTime",
                                             "Message", "Odometer", "LapsLeft", "It", "BattleBar",
                                             "BattleWeapon", "BattleCoin", "BattlePowerup" };
        static_assert(sizeof(names) / sizeof(names[0]) == (size_t)Role::Count);
        return (size_t)role < (size_t)Role::Count ? names[(size_t)role] : "?";
    }

    const Texture* texture(const char* name, int language) {
        const Texture* any = nullptr;
        for (const Texture& t : rom_textures) {
            if (std::strcmp(t.name, name) != 0) continue;
            if (t.language == language) return &t;
            if (t.language == -1 || (any == nullptr && t.language == 0)) any = &t;
        }
        return any;
    }

    std::vector<Part> parts(const Variant& v) {
        std::vector<Part> out;
        int players = v.players < 1 ? 1 : v.players > 4 ? 4 : v.players;
        // The stunt panel's boxes (hud_race_setup_widgets_800A06F8, 0x800A0944-0x800A0A80): with one player at
        // y 180-220, else player 1's at y 70-110 and player 2's (players 3 and 4's copies) at 180-220. Built before
        // the table's elements, kept until the HUD goes.
        auto stunt_boxes = [&](int player, int y0, int y1) {
            box_parts(out, 0xC1, y0, 0x12A, y1, 0x80060E34, (uint32_t)(player << 4), Role::StuntScore, player, 0,
                      0x000000C0, "stunt scores' box (80060E34): the stunt arena, Stunts on All Tracks");
            box_parts(out, 0x16, y0, 0x7F, y1, 0x80060E34, (uint32_t)(player << 4) | 1, Role::StuntTrick, player, 0,
                      0x000000C0, "trick name's box (80060E34): while a trick scores");
            stunt_text(out, player, Rect{ 0xC1, y0, 0x129, y1 - 1 }, Rect{ 0x16, y0, 0x7E, y1 - 1 });
        };
        // BONUS TIME's box, built first (callback 80060F24, parameter 1): 50 higher with two or more players.
        box_parts(out, 0x64, 0x9B, 0xDC, 0xB5, 0x80060F24, 1, Role::Message, -1, players >= 2 ? -50 : 0, 0x000000C0,
                  "BONUS TIME's box (80060F24)");
        for (size_t i = out.size() - 5; i < out.size(); i++) out[i].group = message_bonus;
        if (players == 1) {
            stunt_boxes(0, 0xB4, 0xDC);
        }
        else {
            stunt_boxes(0, 0x46, 0x6E);
            for (int p = 1; p < players; p++) stunt_boxes(p, 0xB4, 0xDC);
        }
        // The layout table, then players 3 and 4's copies of player 2's entries (src/players4.cpp hud_built).
        for (const RomEntry& e : rom_hud_table) {
            int player = entry_player(e.callback, e.param);
            entry_parts(out, e, e.param, player, v);
            if (player >= players) out.back().present = false;  // built, but its callback hides it for good
        }
        for (int p = 2; p < players; p++) {
            for (const RomEntry& e : rom_hud_table) {
                if (!cloned(e.callback) || entry_player(e.callback, e.param) != 1) continue;
                entry_parts(out, e, with_player(e.callback, e.param, p), p, v);
            }
        }
        // The finish overlay (hud_race_finish_overlay_build_800A0FB4), once the race ends. Its boxes come first, so
        // they're drawn behind its images: with the race over (game state 4 or 5) a box behind GAME OVER, x 0x53-0xED,
        // y 0x78-0x9D less 0x35 with one view, else less 0xF (alpha 0xC0, no callback); in a split screen a box
        // behind each finished player's place (src/players4.cpp finish_boxes; the game's own with two stacked views,
        // none with one). Then its table (players 3 and 4's copies of player 2's places).
        {
            int lift = v.views > 1 ? 0xF : 0x35;
            box_parts(out, 0x53, 0x78 - lift, 0xED, 0x9D - lift, 0, 0, Role::Message, -1, 0, 0x000000C0,
                      "GAME OVER's box (800A0FB4): once the race is over");
            for (size_t i = out.size() - 5; i < out.size(); i++) out[i].group = message_game_over;
        }
        if (v.views > 1) {
            for (int p = 0; p < players; p++) {
                // Players 3 and 4's are player 2's with their player as the parameter (finish_boxes).
                int y0 = p == 0 ? 0x1D : 0xAB, y1 = p == 0 ? 0x3F : 0xCD;
                box_parts(out, 0x63, y0, 0xDD, y1, 0, p >= 2 ? (uint32_t)p : 0, Role::Banner, p, 0, 0x000000C0,
                          "finish: the place's box");
            }
        }
        for (const RomEntry& e : rom_finish_table) {
            int player = entry_player(e.callback, e.param);
            entry_parts(out, e, e.param, player, v);
            if (player >= players) out.back().present = false;
        }
        for (int p = 2; p < players; p++) {
            for (const RomEntry& e : rom_finish_table) {
                if (entry_player(e.callback, e.param) != 1) continue;
                entry_parts(out, e, with_player(e.callback, e.param, p), p, v);
            }
        }
        return out;
    }

    int find(const std::vector<Part>& parts, const Identity& id) {
        if (id.kind == Identity::Kind::None) return -1;
        for (size_t i = 0; i < parts.size(); i++) {
            const Identity& p = parts[i].id;
            if (p.kind != id.kind || p.callback != id.callback || p.param != id.param || p.x != id.x || p.y != id.y) continue;
            if (id.kind == Identity::Kind::Entry ? std::strcmp(p.name, id.name) == 0
                                                 : p.x1 == id.x1 && p.y1 == id.y1 && p.part == id.part) {
                return (int)i;
            }
        }
        return -1;
    }
}
