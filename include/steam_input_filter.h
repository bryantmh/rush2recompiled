#ifndef __STEAM_INPUT_FILTER_H__
#define __STEAM_INPUT_FILTER_H__

// Drops the keys and mouse clicks Steam types for a Steam Controller (src/steam_input_filter.cpp).
//
// The game isn't a Steam game, so while Steam runs it plays a Steam Controller (or a Steam Deck's controls in desktop
// mode) through its desktop layout: on top of the controller's own buttons, which the game reads through SDL, Steam
// injects keys and mouse input for them (A Return, B Escape, the d-pad the arrow keys, the bumpers Ctrl and Alt, Y
// Space, Start Escape, the triggers and trackpads mouse clicks). The game then sees every press twice, and Start opens
// the settings menu. While a Valve controller is connected, the game's window ignores injected keys and clicks; real
// keyboards and mice aren't injected, so they keep working. Nothing outside the game's window is affected.
// tools/sdl_devices.py --events shows where presses come from.
namespace rush2::steam_input_filter {
    // Subclasses the game's window (an HWND on Windows; nothing elsewhere). Call on the window's thread.
    void install(void* window);
    // Whether a Valve controller is connected (the input thread keeps this up to date).
    void set_steam_controller_connected(bool connected);
}

#endif
