// Controller light bars in the car colors (include/light_bar.h). Backported from the PS5 port
// (github.com/Marice/rush2-ps5, ps5/ps5_platform.cpp).
//
// A port's light bar shows its player's MAIN COLOR: on the car select screen the MAIN COLOR row of the car in the
// player's slot (rush2_car_rows_frame, src/rush2049/wings_menu.cpp), and in a race the physics car's +0x7EB
// (rush2_rumble_impact, src/rumble.cpp; docs/npc_cars.md). Until a color is chosen the controller keeps the color its
// driver gave it. SDL_GameControllerSetLED does nothing on a controller without a light bar.

#include <array>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "car2049.h"
#include "light_bar.h"
#include "paint_colors.h"
#include "rush2.h"

namespace {
    // The color each port's light bar should show (0: none chosen yet), set from the game thread; and what was last
    // sent, to which controller, on the VI thread.
    constexpr int ports = 4;
    std::array<std::atomic<uint32_t>, ports> wanted{};
    std::array<int32_t, ports> sent_controller{ -1, -1, -1, -1 };
    std::array<uint32_t, ports> sent{};
}

void rush2::light_bar::set(int port, uint32_t rgba) {
    if (port >= 0 && port < ports) {
        wanted[port].store(rgba | 0xFF, std::memory_order_relaxed); // Never 0 once a color is set.
    }
}

void rush2::light_bar::set_car(int port, int type, int color) {
    if (rush2::car2049::paint_jobs(type) > 0) {
        return;
    }
    set(port, rush2::paint_colors::rgba(color));
}

void rush2::light_bar::update() {
    std::lock_guard lock{ rush2::input::sdl_device_mutex() };
    for (int port = 0; port < ports; port++) {
        uint32_t color = wanted[port].load(std::memory_order_relaxed);
        int32_t id = rush2::input::get_port_controller(port);
        if (color == 0 || id < 0 || (id == sent_controller[port] && color == sent[port])) {
            continue;
        }
        // Sent once per change even if it fails, so a pad without a light bar isn't asked again every VI.
        sent_controller[port] = id;
        sent[port] = color;
        if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(id)) {
            SDL_GameControllerSetLED(controller, uint8_t(color >> 24), uint8_t(color >> 16), uint8_t(color >> 8));
        }
    }
}
