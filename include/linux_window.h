#ifndef __LINUX_WINDOW_H__
#define __LINUX_WINDOW_H__

// Keeps GTK dialogs (the file pickers nfd opens, such as Locate ROM) and opened folders in front of the game window
// on Linux (src/linux_window.cpp). Linux only.

#include <filesystem>

namespace rush2::linux_window {
    // Before NFD_Init (which starts GTK): has GTK use X11 when SDL will, so a dialog can belong to the game's window.
    void prepare_gtk();
    // Once the window exists: every GTK dialog from then on is transient for this X11 window (0: none, as under
    // SDL's Wayland driver), which window managers keep above it even when it's fullscreen.
    void set_dialog_parent(unsigned long x11_window);
    // recompui's folder opener (Open Mods Folder and the like): in a gamescope session, starts a file manager as the
    // game's child so it comes up in front of the game, and returns true; elsewhere false, leaving it to xdg-open.
    bool open_folder(const std::filesystem::path& folder);
}

#endif
