#ifndef __NPC_CARS_H__
#define __NPC_CARS_H__

#include <functional>
#include <memory>

namespace recompui {
    class Element;
}
namespace recomp::config {
    class Config;
}
namespace rush2::ui {
    struct ExpandPanel;
}

// The computer cars' car, paint, stripe and rims, chosen per opponent in the General tab's AI Opponents section
// (src/npc_cars.cpp). Every choice starts at Random (or the car's own rims), which keeps Rush 2's random picks.
namespace rush2::npc_cars {
    constexpr int count = 7; // Opponents 1-7: the race's computer cars in car slot order.

    // Registers the hidden options (npc_cars.json). Call before recompui::config::finalize().
    void init_config();
    // Loads the saved choices. Call after recompui::config::finalize().
    void load_config();
    // The choices' config (npc_cars.json), for online lobbies to compare (src/online_tab.cpp).
    recomp::config::Config& config();
    // Adds the collapsible AI Opponents section to a list (the General tab's). on_show, if given, runs when the
    // section's heading is hovered or focused (to show its description). With a panel, the opponents' rows open in
    // the panel's area instead of under the heading.
    void add_section(recompui::Element* parent, std::function<void()> on_show = nullptr,
                     std::shared_ptr<rush2::ui::ExpandPanel> panel = nullptr);
    // What the section does, for the page's description pane.
    extern const char* const description;
    // Drone Cheating: off, the drones' pace no longer depends on the gap to the cars around them.
    void set_catch_up(bool enabled);

    // Game thread. Whether car type `type` can be raced now (Rush 2's, or 2049's while they are offered).
    bool type_available(int type);
    // Game thread, from the car draw hook (func_800A37F4 at 0x800A3E80, after rush2_npc_cars_apply): drone car slot
    // `slot` gets TIRE RIMS `rim` (RIM01 + rim) in this race, as an opponent's chosen rims do.
    void set_slot_rim(int slot, int rim);
}

#endif
