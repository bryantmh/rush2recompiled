// The game's Controller Setup screen, rebuilt around src/controls.cpp's bindings.
//
// The screen (func_800AD7E4, reached from Options and the pause menu) lists 9 actions per player: GAS, BRAKE,
// STEERING, SHIFT UP, SHIFT DOWN, REVERSE, ABORT, VIEW, HORN. Originally each row holds an N64 button code (stored per
// port in 9 bytes at 0x80125A70 + port * 9: 0-8 = A, B, C-up, C-down, C-left, C-right, L, R, Z; 10 = stick;
// 11 = D-pad) that D-left/right cycle, with a "!" on conflicting rows that blocks leaving.
//
// Here the code table is locked to the game's defaults (see src/controls.cpp) and the screen edits the player's real
// bindings instead:
// - A on a row listens for the next input on that player's controller or keyboard and binds it. An input another
//   row uses moves to this row's old input. Start (or Return) cancels listening, as does waiting 6 seconds.
// - Under the rows, SAVE keeps the changes, RESET restores this player's defaults and CANCEL drops the changes since
//   the screen opened. Start also saves and B also cancels. L+R still resets.
// - Each row shows the bound input's button glyph (tools/build_button_glyphs.py), PlayStation glyphs for a
//   PlayStation controller and Xbox glyphs for any other, or keys when the player only has the keyboard.
//
// The screen's own per-frame logic is skipped (hook at the top of its player loop) and replaced by update_player.
// Its widget table (41 entries at 0x800C1130: the binding and conflict icons, the L and R icons, and the text
// callback, built into objects by func_800604FC) is replaced by one holding only the text callback (func_800B6134),
// which draws the labels and, through draw(), everything else. The game's "L+R: DEFAULTS" footer text isn't printed.
//
// With wings enabled (Rush 2049 tab) a tenth row, WINGS, is added: the text callback's label table (0x800C4B08) is
// copied with "WINGS" appended, an instruction patch lets its label loop run 10 rows, and the rows are moved 15
// pixels apart instead of 17 so everything fits in the panel. Whether the row is there is decided when the screen
// opens.
//
// Leaving: from the main menu the screen pops the menu stack itself (as func_800AD7E4 does); from the pause menu,
// func_800AFD44 leaves once rush2_controls_menu_pause_exit says so.

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "button_glyphs.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "wings_internal.h"

extern "C" void func_80064908(uint8_t* rdram, recomp_context* ctx); // Plays a menu sound.
extern "C" void func_80093E08(uint8_t* rdram, recomp_context* ctx); // Lets player 2 join with Start.
extern "C" void func_80088C24(uint8_t* rdram, recomp_context* ctx); // Selects a font.
extern "C" void func_800737E4(uint8_t* rdram, recomp_context* ctx); // Selects a text color style.
extern "C" void func_800732AC(uint8_t* rdram, recomp_context* ctx); // Measures a string.
extern "C" void func_800734E0(uint8_t* rdram, recomp_context* ctx); // Prints a string at (x, y).

using rush2::controls::Action;
using rush2::controls::Device;
using rush2::controls::Glyph;
using rush2::controls::Input;

namespace {
    // Game addresses.
    constexpr uint32_t widget_table = 0x800C1130;
    constexpr int widget_size = 0x28;
    constexpr int text_widget = 40; // The text callback's entry.
    constexpr uint32_t label_table = 0x800C4B08;
    constexpr uint32_t player_structs = 0x800C2140;
    constexpr int player_size = 0x28;
    constexpr uint32_t cursors = 0x800D5438;        // Selected row of each player, halfwords.
    constexpr uint32_t conflict_flags = 0x800D5658; // 9 bytes per player.
    constexpr uint32_t game_bindings = 0x80125A70;  // 9 bytes per port.
    constexpr uint32_t pause_state = 0x8002305C;
    constexpr uint32_t game_mode = 0x8010C0D0;      // Nonzero in a race (the screen was opened from the pause menu).
    constexpr uint32_t language = 0x800C4618;
    constexpr uint32_t menu_current = 0x80023064;
    constexpr uint32_t menu_depth = 0x80023066;
    constexpr uint32_t menu_stack = 0x80023068;
    constexpr uint32_t menu_transition = 0x800E7BB4;
    constexpr uint32_t dl_2d_cursor = 0x80125ABC;   // Next free command in the 2D display list.
    constexpr uint8_t no_port = 5;
    constexpr int num_game_ports = 4;
    constexpr int num_players = 2;

    // The game's default bindings, which the code table is locked to.
    constexpr std::array<int8_t, 9> game_layout = { 0, 1, 10, 7, 8, 3, 2, 6, 5 };

    // Pressed/held N64 buttons in the player struct.
    constexpr uint16_t button_a = 0x8000;
    constexpr uint16_t button_b = 0x4000;
    constexpr uint16_t button_start = 0x1000;
    constexpr uint16_t button_up = 0x0800;
    constexpr uint16_t button_down = 0x0400;
    constexpr uint16_t button_left = 0x0200;
    constexpr uint16_t button_right = 0x0100;
    constexpr uint16_t button_l = 0x0020;
    constexpr uint16_t button_r = 0x0010;
    constexpr uint16_t button_c_left = 0x0002; // WINGS in the game's layout.

    // Menu sounds.
    constexpr int sound_up = 0x3B;
    constexpr int sound_down = 0x44;
    constexpr int sound_change = 0x49;
    constexpr int sound_defaults = 0x66;
    constexpr int sound_confirm = 0x31;
    constexpr int sound_back = 0x32;
    constexpr int sound_start = 0x50;

    // Text: label color styles (0x14 is the game's flashing highlight).
    constexpr int text_normal = 1;
    constexpr int text_selected = 0x14;

    // Layout (in the game's 320x240 screen). Labels are right-aligned at label_right[player]; rows start at
    // first_row_y (2 lower in the pause menu).
    constexpr int label_right[num_players] = { 0x6F - 7, 0xFD - 7 };
    constexpr int panel_center[num_players] = { 0x56, 0xEA };
    constexpr int first_row_y = 0x78 - 0x4A;
    constexpr int first_row_y_paused = 0x78 - 0x48;
    constexpr int original_row_spacing = 17;
    constexpr int wings_row_spacing = 15;
    constexpr int original_rows = 9;
    constexpr int glyph_gap = 4;   // Between a label and its glyph.
    constexpr int glyph_w = 16;    // The size of the game's button icons.
    constexpr int glyph_h = 14;
    constexpr int glyph_y_offset = -4;
    constexpr int footer_gap = 5;

    // Glyph images and the display lists that draw them, in spare RDRAM below 16MB (display list addresses are 24
    // bits). src/interpolation.cpp uses 0x80B00000-0x80C00000, src/track1_audio.cpp 0x80C90000-0x80C99E00,
    // src/widescreen.cpp 0x80CF0000-0x80D00000, src/wings_render.cpp 0x80D00000-0x80E10000.
    constexpr uint32_t glyph_images = 0x80C00000;
    constexpr uint32_t glyph_size = rush2::controls::glyph_width * rush2::controls::glyph_height;
    constexpr uint32_t glyph_dl_start = 0x80C80000;
    constexpr uint32_t glyph_dl_size = 0x4000;
    constexpr int glyph_dl_count = 4; // Rotated each frame so the renderer never reads one being rewritten.

    enum class Footer { Save, Reset, Cancel, Count };
    enum class Exit { None, Save, Cancel };

    struct PlayerState {
        Footer footer = Footer::Save;
    };
    std::array<PlayerState, num_players> player_states{};

    // Whether the open screen has the WINGS row (latched when its widgets are created).
    bool menu_has_wings = false;
    std::atomic_bool pause_exit_pending = false;

    uint32_t new_widget_table = 0;
    uint32_t new_label_table = 0;

    // Strings the screen prints, in the recomp heap.
    struct Strings {
        uint32_t save, reset, cancel, press, left, right, none;
    };
    Strings strings{};

    bool glyphs_loaded = false;
    int glyph_count = 0;
    int glyph_dl_index = 0;

    uint32_t heap_address(uint8_t* rdram, void* ptr) {
        return uint32_t(reinterpret_cast<uint8_t*>(ptr) - rdram) + 0x80000000u;
    }

    uint32_t alloc_string(uint8_t* rdram, const char* s) {
        size_t len = strlen(s) + 1;
        uint32_t addr = heap_address(rdram, recomp::alloc(rdram, (len + 3) & ~3));
        for (size_t i = 0; i < len; i++) {
            MEM_B(0, (int32_t)(addr + i)) = s[i];
        }
        return addr;
    }

    uint8_t player_port(uint8_t* rdram, int player) {
        return MEM_BU(0, (int32_t)(player_structs + player * player_size + 1));
    }

    int row_count() {
        return menu_has_wings ? original_rows + 1 : original_rows;
    }

    int row_spacing() {
        return menu_has_wings ? wings_row_spacing : original_row_spacing;
    }

    // Calls a game function from a hook, keeping the hooked function's registers. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*),
                 int32_t a0 = 0, int32_t a1 = 0, int32_t a2 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    void play_sound(uint8_t* rdram, recomp_context* ctx, int sound) {
        call(rdram, ctx, func_80064908, sound);
    }

    void lock_game_layout(uint8_t* rdram) {
        for (int port = 0; port < num_game_ports; port++) {
            for (int i = 0; i < (int)game_layout.size(); i++) {
                MEM_B(0, (int32_t)(game_bindings + port * 9 + i)) = game_layout[i];
            }
        }
    }

    // The device whose bindings a port's rows show: its controller, or the keyboard if it has none.
    Device shown_device(int port) {
        if (rush2::input::get_port_controller(port) < 0 && rush2::input::get_keyboard_port() == port) {
            return Device::Keyboard;
        }
        return Device::Controller;
    }

    // Copies the widget table's text callback entry into a table of its own.
    void build_widget_table(uint8_t* rdram) {
        new_widget_table = heap_address(rdram, recomp::alloc(rdram, widget_size));
        for (int i = 0; i < widget_size; i += 4) {
            MEM_W(0, (int32_t)(new_widget_table + i)) = MEM_W(0, (int32_t)(widget_table + text_widget * widget_size + i));
        }
    }

    // The 9 action labels plus "WINGS" (only the US language block is needed; the language index is always 0).
    void build_label_table(uint8_t* rdram) {
        uint32_t wings = alloc_string(rdram, "WINGS");
        new_label_table = heap_address(rdram, recomp::alloc(rdram, 10 * 4));
        for (int i = 0; i < 9; i++) {
            MEM_W(0, (int32_t)(new_label_table + i * 4)) = MEM_W(0, (int32_t)(label_table + i * 4));
        }
        MEM_W(0, (int32_t)(new_label_table + 9 * 4)) = (int32_t)wings;
    }

    void load_strings(uint8_t* rdram) {
        if (strings.save != 0) {
            return;
        }
        strings.save = alloc_string(rdram, "SAVE");
        strings.reset = alloc_string(rdram, "RESET");
        strings.cancel = alloc_string(rdram, "CANCEL");
        strings.press = alloc_string(rdram, "PRESS");
        strings.left = alloc_string(rdram, "LEFT");
        strings.right = alloc_string(rdram, "RIGHT");
        strings.none = alloc_string(rdram, "--");
    }

    // Copies assets/button_glyphs.bin into RDRAM.
    void load_glyphs(uint8_t* rdram) {
        if (glyphs_loaded) {
            return;
        }
        glyphs_loaded = true;
        std::ifstream in{ recompui::file::get_program_path() / "assets" / "button_glyphs.bin", std::ios::binary };
        std::vector<uint8_t> data{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        auto be32 = [&](size_t o) {
            return (uint32_t(data[o]) << 24) | (uint32_t(data[o + 1]) << 16) | (uint32_t(data[o + 2]) << 8) | data[o + 3];
        };
        if (data.size() < 16 || memcmp(data.data(), "R2BG", 4) != 0 ||
            be32(8) != rush2::controls::glyph_width || be32(12) != rush2::controls::glyph_height) {
            printf("[Controls] Missing or invalid assets/button_glyphs.bin\n");
            return;
        }
        uint32_t count = std::min<uint32_t>(be32(4), (uint32_t)Glyph::Count);
        if (data.size() < 16 + count * glyph_size) {
            printf("[Controls] Truncated assets/button_glyphs.bin\n");
            return;
        }
        for (uint32_t i = 0; i < count * glyph_size; i++) {
            MEM_B(0, (int32_t)(glyph_images + i)) = data[16 + i];
        }
        glyph_count = (int)count;
    }

    struct GlyphColor {
        Glyph glyph;
        uint32_t rgba;
    };

    constexpr uint32_t white = 0xFFFFFFFF;

    // The glyph (and tint) for a bound input.
    GlyphColor glyph_for(const Input& in, bool playstation) {
        using G = Glyph;
        switch (in.type) {
            case Input::Type::Button:
                switch (in.id) {
                    case SDL_CONTROLLER_BUTTON_A: return playstation ? GlyphColor{ G::SonyCross, 0x7CB2E8FF } : GlyphColor{ G::XboxA, 0x6CC24AFF };
                    case SDL_CONTROLLER_BUTTON_B: return playstation ? GlyphColor{ G::SonyCircle, 0xF2626BFF } : GlyphColor{ G::XboxB, 0xF2504BFF };
                    case SDL_CONTROLLER_BUTTON_X: return playstation ? GlyphColor{ G::SonySquare, 0xE59AD8FF } : GlyphColor{ G::XboxX, 0x4D8FEBFF };
                    case SDL_CONTROLLER_BUTTON_Y: return playstation ? GlyphColor{ G::SonyTriangle, 0x43C6A5FF } : GlyphColor{ G::XboxY, 0xF5C342FF };
                    case SDL_CONTROLLER_BUTTON_BACK: return { playstation ? G::SonyCreate : G::XboxView, white };
                    case SDL_CONTROLLER_BUTTON_GUIDE: return { G::Home, white };
                    case SDL_CONTROLLER_BUTTON_START: return { playstation ? G::SonyOptions : G::XboxMenu, white };
                    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return { G::LeftStickClick, white };
                    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return { G::RightStickClick, white };
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return { playstation ? G::SonyL1 : G::XboxLB, white };
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return { playstation ? G::SonyR1 : G::XboxRB, white };
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: return { G::DpadUp, white };
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return { G::DpadDown, white };
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return { G::DpadLeft, white };
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return { G::DpadRight, white };
                    case SDL_CONTROLLER_BUTTON_MISC1: return { G::Misc, white };
                    case SDL_CONTROLLER_BUTTON_PADDLE1: return { G::PaddleR4, white };
                    case SDL_CONTROLLER_BUTTON_PADDLE2: return { G::PaddleL4, white };
                    case SDL_CONTROLLER_BUTTON_PADDLE3: return { G::PaddleR5, white };
                    case SDL_CONTROLLER_BUTTON_PADDLE4: return { G::PaddleL5, white };
                    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return { G::SonyTouchpad, white };
                }
                break;
            case Input::Type::AxisPositive:
            case Input::Type::AxisNegative: {
                bool pos = in.type == Input::Type::AxisPositive;
                switch (in.id) {
                    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return { playstation ? G::SonyL2 : G::XboxLT, white };
                    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return { playstation ? G::SonyR2 : G::XboxRT, white };
                    case SDL_CONTROLLER_AXIS_LEFTX: return { pos ? G::LeftStickRight : G::LeftStickLeft, white };
                    case SDL_CONTROLLER_AXIS_LEFTY: return { pos ? G::LeftStickDown : G::LeftStickUp, white };
                    case SDL_CONTROLLER_AXIS_RIGHTX: return { pos ? G::RightStickRight : G::RightStickLeft, white };
                    case SDL_CONTROLLER_AXIS_RIGHTY: return { pos ? G::RightStickDown : G::RightStickUp, white };
                }
                break;
            }
            case Input::Type::Stick:
                return { in.id == rush2::controls::RightStick ? G::RightStick : in.id == rush2::controls::Dpad ? G::Dpad : G::LeftStick, white };
            case Input::Type::Key: {
                int k = in.id;
                if (k >= SDL_SCANCODE_A && k <= SDL_SCANCODE_Z) return { Glyph((int)G::KeyA + (k - SDL_SCANCODE_A)), white };
                if (k >= SDL_SCANCODE_1 && k <= SDL_SCANCODE_9) return { Glyph((int)G::Key1 + (k - SDL_SCANCODE_1)), white };
                if (k >= SDL_SCANCODE_F1 && k <= SDL_SCANCODE_F12) return { Glyph((int)G::KeyF1 + (k - SDL_SCANCODE_F1)), white };
                if (k >= SDL_SCANCODE_KP_1 && k <= SDL_SCANCODE_KP_9) return { Glyph((int)G::KeyPad1 + (k - SDL_SCANCODE_KP_1)), white };
                switch (k) {
                    case SDL_SCANCODE_0: return { G::Key0, white };
                    case SDL_SCANCODE_KP_0: return { G::KeyPad0, white };
                    case SDL_SCANCODE_UP: return { G::KeyUp, white };
                    case SDL_SCANCODE_DOWN: return { G::KeyDown, white };
                    case SDL_SCANCODE_LEFT: return { G::KeyLeft, white };
                    case SDL_SCANCODE_RIGHT: return { G::KeyRight, white };
                    case SDL_SCANCODE_SPACE: return { G::KeySpace, white };
                    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return { G::KeyShift, white };
                    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return { G::KeyControl, white };
                    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return { G::KeyAlt, white };
                    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return { G::KeySuper, white };
                    case SDL_SCANCODE_TAB: return { G::KeyTab, white };
                    case SDL_SCANCODE_CAPSLOCK: return { G::KeyCaps, white };
                    case SDL_SCANCODE_RETURN: return { G::KeyEnter, white };
                    case SDL_SCANCODE_BACKSPACE: return { G::KeyBackspace, white };
                    case SDL_SCANCODE_ESCAPE: return { G::KeyEscape, white };
                    case SDL_SCANCODE_DELETE: return { G::KeyDelete, white };
                    case SDL_SCANCODE_INSERT: return { G::KeyInsert, white };
                    case SDL_SCANCODE_HOME: return { G::KeyHome, white };
                    case SDL_SCANCODE_END: return { G::KeyEnd, white };
                    case SDL_SCANCODE_PAGEUP: return { G::KeyPageUp, white };
                    case SDL_SCANCODE_PAGEDOWN: return { G::KeyPageDown, white };
                    case SDL_SCANCODE_MINUS: return { G::KeyMinus, white };
                    case SDL_SCANCODE_EQUALS: return { G::KeyEquals, white };
                    case SDL_SCANCODE_LEFTBRACKET: return { G::KeyLeftBracket, white };
                    case SDL_SCANCODE_RIGHTBRACKET: return { G::KeyRightBracket, white };
                    case SDL_SCANCODE_BACKSLASH: return { G::KeyBackslash, white };
                    case SDL_SCANCODE_SEMICOLON: return { G::KeySemicolon, white };
                    case SDL_SCANCODE_APOSTROPHE: return { G::KeyApostrophe, white };
                    case SDL_SCANCODE_GRAVE: return { G::KeyGrave, white };
                    case SDL_SCANCODE_COMMA: return { G::KeyComma, white };
                    case SDL_SCANCODE_PERIOD: return { G::KeyPeriod, white };
                    case SDL_SCANCODE_SLASH: return { G::KeySlash, white };
                    case SDL_SCANCODE_KP_PLUS: return { G::KeyPadPlus, white };
                    case SDL_SCANCODE_KP_MINUS: return { G::KeyPadMinus, white };
                    case SDL_SCANCODE_KP_MULTIPLY: return { G::KeyPadMultiply, white };
                    case SDL_SCANCODE_KP_DIVIDE: return { G::KeyPadDivide, white };
                    case SDL_SCANCODE_KP_ENTER: return { G::KeyPadEnter, white };
                    case SDL_SCANCODE_KP_PERIOD: return { G::KeyPadPeriod, white };
                }
                return { G::KeyUnknown, white };
            }
            default:
                break;
        }
        return { Glyph::None, white };
    }

    // Builds a display list of textured rectangles drawing glyphs, called from the game's 2D display list.
    class GlyphList {
    public:
        explicit GlyphList(uint8_t* rdram) : rdram(rdram) {
            start = glyph_dl_start + glyph_dl_index * glyph_dl_size;
            glyph_dl_index = (glyph_dl_index + 1) % glyph_dl_count;
            cursor = start;
        }

        void add(Glyph glyph, uint32_t rgba, int x, int y) {
            if ((int)glyph >= glyph_count || cursor + 0x100 > start + glyph_dl_size) {
                return;
            }
            if (!begun) {
                begin();
            }
            constexpr uint32_t w = rush2::controls::glyph_width;
            constexpr uint32_t h = rush2::controls::glyph_height;
            constexpr uint32_t fmt_i = 4, siz_8b = 1, siz_16b = 2;
            constexpr uint32_t clamp = 2;
            uint32_t image = (glyph_images + (uint32_t)glyph * glyph_size) & 0x1FFFFFFF;
            uint32_t tile_clamp = (clamp << 18) | (clamp << 8);

            cmd(0xFA000000, rgba);                                                  // SetPrimColor
            cmd(0xFD000000 | (fmt_i << 21) | (siz_16b << 19), image);               // SetTextureImage (8-bit as 16-bit)
            cmd(0xF5000000 | (fmt_i << 21) | (siz_16b << 19), (7u << 24) | tile_clamp); // SetTile: load tile
            cmd(0xE6000000, 0);                                                     // LoadSync
            uint32_t texels = (w * h + 1) / 2 - 1;
            uint32_t dxt = (2048 + w / 8 - 1) / (w / 8);
            cmd(0xF3000000, (7u << 24) | (texels << 12) | dxt);                     // LoadBlock
            cmd(0xE7000000, 0);                                                     // PipeSync
            cmd(0xF5000000 | (fmt_i << 21) | (siz_8b << 19) | (((w + 7) / 8) << 9), tile_clamp); // SetTile: tile 0
            cmd(0xF2000000, (((w - 1) << 2) << 12) | ((h - 1) << 2));               // SetTileSize
            uint32_t ulx = x * 4, uly = y * 4, lrx = (x + glyph_w) * 4, lry = (y + glyph_h) * 4;
            cmd(0xE4000000 | (lrx << 12) | lry, (ulx << 12) | uly);                 // TextureRectangle
            cmd(0xE1000000, 0);                                                     // s, t = 0
            uint32_t dsdx = (w << 10) / glyph_w, dtdy = (h << 10) / glyph_h;
            cmd(0xF1000000, (dsdx << 16) | dtdy);
        }

        // Ends the list and calls it from the game's 2D display list.
        void finish() {
            if (!begun) {
                return;
            }
            GfxCommand cmds[4];
            cmd(0xE7000000, 0);
            gEXPopCombineMode(&cmds[0]);
            gEXPopOtherMode(&cmds[1]);
            gEXPopPrimColor(&cmds[2]);
            for (int i = 0; i < 3; i++) {
                cmd(cmds[i].values.word0, cmds[i].values.word1);
            }
            cmd(0xDF000000, 0); // EndDL

            int32_t dl = MEM_W(0, (int32_t)dl_2d_cursor);
            if (dl == 0) {
                return;
            }
            MEM_W(0, dl) = (int32_t)0xDE000000;                    // DL call
            MEM_W(4, dl) = (int32_t)(start & 0x1FFFFFFF);
            MEM_W(0, (int32_t)dl_2d_cursor) = dl + 8;
        }

    private:
        uint8_t* rdram;
        uint32_t start;
        uint32_t cursor;
        bool begun = false;

        void cmd(uint32_t w0, uint32_t w1) {
            MEM_W(0, (int32_t)cursor) = (int32_t)w0;
            MEM_W(4, (int32_t)cursor) = (int32_t)w1;
            cursor += 8;
        }

        void begin() {
            begun = true;
            GfxCommand cmds[4];
            gEXEnable(&cmds[0]);
            gEXPushPrimColor(&cmds[1]);
            gEXPushOtherMode(&cmds[2]);
            gEXPushCombineMode(&cmds[3]);
            cmd(0xE7000000, 0); // PipeSync
            for (const GfxCommand& c : cmds) {
                cmd(c.values.word0, c.values.word1);
            }
            // 1-cycle, no perspective, bilinear, no LUT, filtered texture conversion, no dithering. Translucent
            // surface blending, no Z.
            cmd(0xEF000000 | 0x002CF0, 0x00504240); // SetOtherMode
            // Color = primitive, alpha = texel (the glyph's intensity) * primitive alpha, in both cycles.
            uint32_t c_sa = 15, c_sb = 15, c_m = 31, c_a = 3;
            uint32_t a_sa = 1, a_sb = 7, a_m = 3, a_a = 7;
            uint32_t w0 = (c_sa << 20) | (c_m << 15) | (a_sa << 12) | (a_m << 9) | (c_sa << 5) | c_m;
            uint32_t w1 = (c_sb << 28) | (c_a << 15) | (a_sb << 12) | (a_a << 9) |
                (c_sb << 24) | (a_sa << 21) | (a_m << 18) | (c_a << 6) | (a_sb << 3) | a_a;
            cmd(0xFC000000 | w0, w1); // SetCombine
        }
    };

    // Text helpers. Each call keeps the caller's registers.
    void set_font(uint8_t* rdram, recomp_context* ctx) {
        int font = MEM_W(0, (int32_t)language) == 1 ? 6 : 8; // The font the labels use.
        call(rdram, ctx, func_80088C24, font);
    }

    int text_width(uint8_t* rdram, recomp_context* ctx, uint32_t str) {
        return call(rdram, ctx, func_800732AC, (int32_t)str, -1);
    }

    void print(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, uint32_t str) {
        call(rdram, ctx, func_800737E4, style);
        call(rdram, ctx, func_800734E0, x, y, (int32_t)str);
    }

    Exit update_player(uint8_t* rdram, recomp_context* ctx, int player, int port) {
        int32_t cursor_addr = (int32_t)(cursors + player * 2);
        int cursor = MEM_H(0, cursor_addr);
        int footer_row = row_count();
        PlayerState& state = player_states[player];

        if (rush2::controls::is_listening(port)) {
            if (rush2::controls::update_listen(port)) {
                play_sound(rdram, ctx, sound_change);
            }
            return Exit::None;
        }

        uint32_t player_struct = player_structs + player * player_size;
        uint16_t pressed = MEM_HU(2, (int32_t)player_struct);
        uint16_t held = MEM_HU(4, (int32_t)player_struct);

        if (pressed & button_up) {
            cursor = cursor <= 0 ? footer_row : cursor - 1;
            play_sound(rdram, ctx, sound_up);
        }
        else if (pressed & button_down) {
            cursor = cursor >= footer_row ? 0 : cursor + 1;
            play_sound(rdram, ctx, sound_down);
        }
        if (cursor > footer_row) {
            cursor = footer_row;
        }
        MEM_H(0, cursor_addr) = (int16_t)cursor;

        bool reset = ((pressed & button_l) && (held & button_r)) || ((pressed & button_r) && (held & button_l));

        if (cursor == footer_row) {
            int count = (int)Footer::Count;
            if (pressed & button_left) {
                state.footer = Footer(((int)state.footer + count - 1) % count);
                play_sound(rdram, ctx, sound_change);
            }
            else if (pressed & button_right) {
                state.footer = Footer(((int)state.footer + 1) % count);
                play_sound(rdram, ctx, sound_change);
            }
            if (pressed & button_a) {
                switch (state.footer) {
                    case Footer::Save: return Exit::Save;
                    case Footer::Cancel: return Exit::Cancel;
                    default: reset = true; break;
                }
            }
        }
        else if (pressed & button_a) {
            rush2::controls::begin_listen(port, Action(cursor));
            play_sound(rdram, ctx, sound_confirm);
            return Exit::None;
        }

        if (reset) {
            rush2::controls::reset_defaults(port, Device::Controller);
            rush2::controls::reset_defaults(port, Device::Keyboard);
            play_sound(rdram, ctx, sound_defaults);
        }
        if (pressed & button_start) {
            return Exit::Save;
        }
        if (pressed & button_b) {
            return Exit::Cancel;
        }
        return Exit::None;
    }

    void leave(uint8_t* rdram, recomp_context* ctx, Exit exit) {
        for (int port = 0; port < rush2::input::num_ports; port++) {
            rush2::controls::cancel_listen(port);
        }
        if (exit == Exit::Save) {
            rush2::controls::save();
            play_sound(rdram, ctx, sound_start);
        }
        else {
            rush2::controls::restore_snapshot();
            play_sound(rdram, ctx, sound_back);
        }

        if (MEM_W(0, (int32_t)game_mode) != 0) {
            // func_800AFD44 leaves the pause menu's screen.
            pause_exit_pending = true;
            return;
        }
        // Back to the previous menu, as func_800AD7E4 does.
        int16_t depth = MEM_H(0, (int32_t)menu_depth) - 1;
        MEM_W(0, (int32_t)menu_transition) = 5;
        MEM_H(0, (int32_t)menu_depth) = depth;
        MEM_H(0, (int32_t)menu_current) = MEM_H(0, (int32_t)(menu_stack + depth * 2));
    }

    void draw_player(uint8_t* rdram, recomp_context* ctx, GlyphList& list, int player, int port) {
        bool paused = MEM_W(0, (int32_t)game_mode) != 0;
        int y0 = paused ? first_row_y_paused : first_row_y;
        int spacing = row_spacing();
        int rows = row_count();
        Device device = shown_device(port);
        bool playstation = rush2::input::port_has_playstation_controller(port);
        int x = label_right[player] + glyph_gap;
        int cursor = MEM_H(0, (int32_t)(cursors + player * 2));
        bool listening = rush2::controls::is_listening(port);

        for (int row = 0; row < rows; row++) {
            int y = y0 + spacing * row;
            if (listening && row == cursor) {
                // The keyboard's steering is bound as a left key and then a right key.
                uint32_t prompt = strings.press;
                if (row == (int)Action::Steering && device == Device::Keyboard) {
                    prompt = rush2::controls::listen_slot(port) == 0 ? strings.left : strings.right;
                }
                print(rdram, ctx, text_selected, x, y, prompt);
                continue;
            }

            int slots = (row == (int)Action::Steering && device == Device::Keyboard) ? 2 : 1;
            bool any = false;
            for (int slot = 0; slot < slots; slot++) {
                Input in = rush2::controls::get_binding(port, device, Action(row), slot);
                GlyphColor g = glyph_for(in, playstation);
                if (g.glyph != Glyph::None) {
                    list.add(g.glyph, g.rgba, x + slot * (glyph_w + 2), y + glyph_y_offset);
                    any = true;
                }
            }
            if (!any) {
                print(rdram, ctx, text_normal, x, y, strings.none);
            }
        }

        // SAVE  RESET  CANCEL, centered under the rows where the game printed "L+R: DEFAULTS".
        int footer_y = y0 + spacing * rows + 3;
        const uint32_t items[] = { strings.save, strings.reset, strings.cancel };
        int widths[3];
        int total = footer_gap * 2;
        for (int i = 0; i < 3; i++) {
            widths[i] = text_width(rdram, ctx, items[i]);
            total += widths[i];
        }
        int fx = panel_center[player] - total / 2;
        bool on_footer = cursor == rows && !listening;
        for (int i = 0; i < 3; i++) {
            bool selected = on_footer && (int)player_states[player].footer == i;
            print(rdram, ctx, selected ? text_selected : text_normal, fx, footer_y, items[i]);
            fx += widths[i] + footer_gap;
        }
    }
}

bool rush2::wings::button_held(uint8_t* rdram, int player) {
    uint8_t port = player_port(rdram, player);
    if (port >= num_game_ports) {
        return false;
    }
    // WINGS is C-left in the game's layout (src/controls.cpp).
    uint16_t held = MEM_HU(0, (int32_t)(player_structs + player * player_size + 4));
    return MEM_B(0, (int32_t)pause_state) == 0 && (held & button_c_left) != 0;
}

// Top of the game thread's main loop (func_800B0228), once per frame in menus and races.
extern "C" void rush2_controls_frame(uint8_t* rdram, recomp_context* ctx) {
    rush2::input::set_rdram(rdram);
    lock_game_layout(rdram);
}

// func_800AD7E4, before the widget list is created ($a2 = table; the count in $a3 is set here, its instruction is
// patched out). The screen is opening.
extern "C" void rush2_controls_menu_widgets(uint8_t* rdram, recomp_context* ctx) {
    menu_has_wings = rush2::wings::enabled();
    if (new_widget_table == 0) {
        build_widget_table(rdram);
    }
    load_strings(rdram);
    load_glyphs(rdram);
    ctx->r6 = (int32_t)new_widget_table;
    ctx->r7 = 1;

    player_states = {};
    pause_exit_pending = false;
    rush2::controls::snapshot();
}

// func_800AD7E4, each frame before its player loop (after the screen's widgets exist). Runs the screen for both
// players; returns true to skip the game's own loop.
extern "C" int rush2_controls_menu_update(uint8_t* rdram, recomp_context* ctx) {
    lock_game_layout(rdram);
    for (int i = 0; i < num_players * 9; i++) {
        MEM_B(0, (int32_t)(conflict_flags + i)) = 0;
    }

    Exit exit = Exit::None;
    for (int player = 0; player < num_players; player++) {
        uint8_t port = player_port(rdram, player);
        if (port == no_port) {
            // An empty player slot: player 2 joins by pressing Start (main menu only, like the game).
            if (MEM_W(0, (int32_t)game_mode) == 0) {
                call(rdram, ctx, func_80093E08);
            }
            continue;
        }
        if (port >= rush2::input::num_ports || exit != Exit::None) {
            continue;
        }
        exit = update_player(rdram, ctx, player, port);
    }

    if (exit != Exit::None) {
        leave(rdram, ctx, exit);
    }
    return 1;
}

// func_800AFD44 (pause menu), at its check for leaving the Controller Setup screen ($t9 = B or Start pressed).
// Returns whether to leave this frame.
extern "C" int rush2_controls_menu_pause_exit(uint8_t* rdram, recomp_context* ctx) {
    return pause_exit_pending.exchange(false) ? 1 : 0;
}

// func_800B6134 (text callback), after both players' labels. Draws the bindings and footers.
extern "C" void rush2_controls_menu_draw(uint8_t* rdram, recomp_context* ctx) {
    set_font(rdram, ctx);
    GlyphList list{ rdram };
    for (int player = 0; player < num_players; player++) {
        uint8_t port = player_port(rdram, player);
        if (port < rush2::input::num_ports) {
            draw_player(rdram, ctx, list, player, port);
        }
    }
    list.finish();
}

// func_800B6134, after it loaded the label table into $s6.
extern "C" void rush2_wings_menu_labels(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings) {
        return;
    }
    if (new_label_table == 0) {
        build_label_table(rdram);
    }
    ctx->r22 = (int32_t)new_label_table;
}

// func_800B6134, at the end of each label row, after the row counter ($s3) was incremented and before the loop
// test (patched to 10 rows) and the row step of 17 pixels in $s2. Ends the loop after 9 rows without wings, and
// makes the step 15 pixels with them.
extern "C" void rush2_wings_menu_label_row(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings) {
        if ((int32_t)ctx->r19 >= original_rows) {
            ctx->r19 = original_rows + 1;
        }
        return;
    }
    ctx->r18 = (int32_t)ctx->r18 - (original_row_spacing - wings_row_spacing);
}
