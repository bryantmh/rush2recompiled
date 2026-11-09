#ifndef __NPC_CARS_H__
#define __NPC_CARS_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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
    // Unlocked Cars as Opponents: on, the drones' random car also draws from the hidden Rush 2 cars (types 16-21)
    // the car select offered for the race.
    void set_unlocked_opponents(bool enabled);
    // Game thread, from the car select's car lists (src/cheats.cpp, after the unlock rules): bit t set for each
    // hidden Rush 2 car type t (16-21) any player was offered.
    void set_offered_cars(uint32_t mask);
    // Game thread, from the drone car draw (src/rush2049/car2049.cpp): the hidden Rush 2 car types the drones may
    // draw this race (none with the option off, in an online race, or before a car select).
    std::vector<int> unlocked_opponent_types();

    // Game thread. Whether car type `type` can be raced now (Rush 2's, or 2049's while they are offered).
    bool type_available(int type);
    // Game thread, from the car draw hook (func_800A37F4 at 0x800A3E80, after rush2_npc_cars_apply): drone car slot
    // `slot` gets TIRE RIMS `rim` (RIM01 + rim) in this race, as an opponent's chosen rims do.
    void set_slot_rim(int slot, int rim);
}

#endif
