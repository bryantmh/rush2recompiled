// Keeps GTK dialogs in front of the game window on Linux (include/linux_window.h).
//
// nfd (the copy in RT64's contrib, without newer nfd's parent window argument) opens its GTK file chooser with no
// parent, so window managers stack it as an unrelated window: behind the game when the game is fullscreen. On Windows
// the dialog belongs to the app and comes up in front. Here an emission hook on GtkWidget's "map" signal makes each
// dialog transient for the game's X11 window just before it's shown, which every X11 window manager keeps above its
// parent. GTK and SDL must be on the same display system for that: SDL2 uses X11 (XWayland under a Wayland desktop)
// unless SDL_VIDEODRIVER says otherwise, so GTK is told to as well.

#ifdef __linux__

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "linux_window.h"

namespace {
    std::atomic<unsigned long> parent_window = 0;

    gboolean on_map(GSignalInvocationHint*, guint, const GValue* params, gpointer) {
#ifdef GDK_WINDOWING_X11
        GObject* object = (GObject*)g_value_get_object(&params[0]);
        unsigned long parent = parent_window.load();
        if (parent != 0 && GTK_IS_DIALOG(object)) {
            GtkWidget* dialog = GTK_WIDGET(object);
            GdkDisplay* display = gtk_widget_get_display(dialog);
            GdkWindow* window = gtk_widget_get_window(dialog);
            if (GDK_IS_X11_DISPLAY(display) && window != nullptr) {
                GdkWindow* game = gdk_x11_window_foreign_new_for_display(display, parent);
                if (game != nullptr) {
                    gdk_window_set_transient_for(window, game);
                    g_object_unref(game);
                }
            }
        }
#endif
        return TRUE; // Stay installed.
    }
}

void rush2::linux_window::prepare_gtk() {
    const char* sdl_driver = getenv("SDL_VIDEODRIVER");
    if (sdl_driver == nullptr || sdl_driver[0] == '\0' || strstr(sdl_driver, "x11") == sdl_driver) {
        setenv("GDK_BACKEND", "x11", 0); // A GDK_BACKEND the player set wins.
    }
}

void rush2::linux_window::set_dialog_parent(unsigned long x11_window) {
    static bool hooked = false;
    parent_window = x11_window;
    if (!hooked && x11_window != 0) {
        hooked = true;
        // GtkWidget's class has to exist before its signal can be looked up (GTK is initialized by NFD_Init).
        g_type_class_unref(g_type_class_ref(GTK_TYPE_WIDGET));
        guint map_signal = g_signal_lookup("map", GTK_TYPE_WIDGET);
        if (map_signal != 0) {
            g_signal_add_emission_hook(map_signal, 0, on_map, nullptr, nullptr);
        }
    }
}

#endif
