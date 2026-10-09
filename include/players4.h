#ifndef __PLAYERS4_H__
#define __PLAYERS4_H__

#include <cstdint>

#include "recomp.h"

// Three and four player races (src/players4.cpp).
namespace rush2::players4 {
    constexpr int max_players = 4;

    // Game variables.
    constexpr uint32_t num_players = 0x8010C3E2; // s16
    constexpr uint32_t num_views = 0x8010C3EC;   // s16

    // RDRAM for this feature's own data, after the relocated arrays (tools/players4/relocate.py, 0x80240000-).
    // The number of players clamped to 2, which us.toml points reads of tables sized for 1-2 players at.
    constexpr uint32_t clamped_players = 0x8024F000; // s16
    constexpr uint32_t viewports = 0x8024F010;       // Vp for views 2 and 3, 0x10 each.
    constexpr uint32_t scissors = 0x8024F030;        // u16 ulx, uly, lrx, lry for views 2 and 3.
    constexpr uint32_t side_dl_start = 0x8024C000;   // Ring of small display lists (src/splitscreen.cpp).
    constexpr uint32_t side_dl_end = 0x8024F000;
    // 0x80250000-0x802D0000: the frames' graphics buffers; 0x802D0000-0x802E9000: their view vertex buffers (us.toml).

    // The address of a player's record (0x28 bytes: +0 car, +1 controller port or 5).
    int32_t record(int player);

    // Called every frame (from the frame setup): keeps clamped_players current.
    void frame(uint8_t* rdram);

    // After the race HUD is built (func_800A06F8): adds players 3 and 4's elements and notes each widget's player.
    void hud_built(uint8_t* rdram, recomp_context* ctx);
    // Players in the menus, counting players 3 and 4 who have joined (the game's count stays at 2 until the race).
    int joined_players(uint8_t* rdram);
    // Draws the menus' "press START" hint for players 3 and 4 (after the widget draw loop, func_8007D9DC).
    void draw_join_hint(uint8_t* rdram, recomp_context* ctx);

    // After the race's finish overlay is built (func_800A0FB4, list head): adds players 3 and 4's and notes players.
    void finish_overlay_built(uint8_t* rdram, recomp_context* ctx, uint32_t head);

    // As the finish overlay is rebuilt in a split screen (rush2_hud_finish_begin): the dark box behind each finished
    // player's place, as the game places them top and bottom, tagged as part of the place so the HUD layout moves
    // them together (src/hud.cpp).
    void finish_boxes(uint8_t* rdram, recomp_context* ctx);

    // The player (0-3) who last opened the pause menu.
    int paused_player();

    // The player a race HUD widget belongs to, or -1 for shared ones.
    int hud_widget_player(int slot);
    // What a race HUD widget is part of, for the split screen layouts (src/hud.cpp).
    enum class HudRole { Other, Time, Speed, Position, Radar, Banner, Deaths, TimeLeft, Map, Tach, Gear, Count };
    HudRole hud_widget_role(int slot);
    // Whether a race HUD widget is part of a player's lap (checkpoint) time box, under their race time (HudRole::Time).
    bool hud_widget_lap_time(int slot);
}

#endif
