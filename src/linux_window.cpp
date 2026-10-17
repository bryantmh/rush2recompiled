// Keeps GTK dialogs in front of the game window on Linux (include/linux_window.h).
//
// nfd (the copy in RT64's contrib, without newer nfd's parent window argument) opens its GTK file chooser with no
// parent, so window managers stack it as an unrelated window: behind the game when the game is fullscreen. On Windows
// the dialog belongs to the app and comes up in front. Here an emission hook on GtkWidget's "map" signal makes each
// dialog transient for the game's X11 window just before it's shown, which every X11 window manager keeps above its
// parent. GTK and SDL must be on the same display system for that: SDL2 uses X11 (XWayland under a Wayland desktop)
// unless SDL_VIDEODRIVER says otherwise, so GTK is told to as well.
//
// open_folder: in a gamescope session (SteamOS's Game Mode and the like) gamescope shows only the windows of the
// running game, which it finds by walking each window's process up to Steam's launcher, and xdg-open there doesn't know
// the desktop, so it opens whatever app lists folders first (a flatpak text editor, say), started outside the game.
// So the game picks a file manager itself and starts it as its own child: it inherits the game's display and process
// tree, and gamescope puts it in front of the game like the file picker.

#ifdef __linux__

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gio/gdesktopappinfo.h>
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

    bool is_file_manager(GAppInfo* app) {
        if (app == nullptr || !G_IS_DESKTOP_APP_INFO(app)) {
            return false;
        }
        const char* categories = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
        if (categories == nullptr) {
            return false;
        }
        gchar** list = g_strsplit(categories, ";", -1);
        bool found = g_strv_contains(list, "FileManager");
        g_strfreev(list);
        return found;
    }

    // The default app for folders when it's a file manager, else the first file manager that opens folders.
    GAppInfo* find_file_manager() {
        GAppInfo* app = g_app_info_get_default_for_type("inode/directory", FALSE);
        if (is_file_manager(app)) {
            return app;
        }
        if (app != nullptr) {
            g_object_unref(app);
            app = nullptr;
        }
        GList* all = g_app_info_get_all_for_type("inode/directory");
        for (GList* it = all; it != nullptr; it = it->next) {
            if (is_file_manager(G_APP_INFO(it->data))) {
                app = G_APP_INFO(g_object_ref(it->data));
                break;
            }
        }
        g_list_free_full(all, g_object_unref);
        return app;
    }

    // The app's Exec line with its file field codes (%f %F %u %U) replaced by the folder, the others dropped. Run
    // straight from the Exec line rather than through GIO, which would start a D-Bus activatable app (Nautilus) from
    // the session bus, outside the game.
    std::vector<std::string> command_for(GAppInfo* app, const std::string& folder) {
        std::vector<std::string> command;
        const char* exec = g_app_info_get_commandline(app);
        gchar** argv = nullptr;
        if (exec == nullptr || !g_shell_parse_argv(exec, nullptr, &argv, nullptr)) {
            return command;
        }
        gchar* uri = g_filename_to_uri(folder.c_str(), nullptr, nullptr);
        bool placed = false;
        for (gchar** arg = argv; *arg != nullptr; arg++) {
            std::string_view text = *arg;
            if (text == "%f" || text == "%F" || text == "%u" || text == "%U") {
                if (!placed) {
                    command.push_back((text == "%u" || text == "%U") && uri != nullptr ? std::string{ uri } : folder);
                    placed = true;
                }
                continue;
            }
            std::string out;
            for (size_t i = 0; i < text.size(); i++) {
                if (text[i] == '%' && i + 1 < text.size()) {
                    if (text[i + 1] == '%') {
                        out += '%';
                    }
                    i++; // %i %c %k and the like have no use here.
                }
                else {
                    out += text[i];
                }
            }
            if (!out.empty()) {
                command.push_back(out);
            }
        }
        if (!placed) {
            command.push_back(folder);
        }
        g_free(uri);
        g_strfreev(argv);
        return command;
    }

    bool start(const std::vector<std::string>& command) {
        if (command.empty()) {
            return false;
        }
        std::vector<gchar*> argv;
        for (const std::string& arg : command) {
            argv.push_back(const_cast<gchar*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        GError* error = nullptr;
        // Without G_SPAWN_DO_NOT_REAP_CHILD, GLib reaps the child itself.
        if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
            fprintf(stderr, "[folder] couldn't start %s: %s\n", command[0].c_str(), error->message);
            g_error_free(error);
            return false;
        }
        return true;
    }
}

bool rush2::linux_window::open_folder(const std::filesystem::path& folder) {
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    if (desktop == nullptr || strstr(desktop, "gamescope") == nullptr) {
        return false; // A desktop's own xdg-open knows its file manager.
    }
    std::string path = folder.string();
    // Reading the apps' desktop files takes a moment: off the UI thread.
    std::thread([path]() {
        GAppInfo* app = find_file_manager();
        bool started = false;
        if (app != nullptr) {
            started = start(command_for(app, path));
            g_object_unref(app);
        }
        if (!started) {
            fprintf(stderr, "[folder] no file manager found for %s\n", path.c_str());
            start({ "xdg-open", path });
        }
    }).detach();
    return true;
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
