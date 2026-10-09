#ifndef __NPC_CARS_H__
#define __NPC_CARS_H__

namespace recompui {
    class Element;
}

// The computer cars' car, paint, stripe and rims, chosen per opponent in the Players tab's AI Opponents section
// (src/npc_cars.cpp). Every choice starts at Random (or the car's own rims), which keeps Rush 2's random picks.
namespace rush2::npc_cars {
    constexpr int count = 7; // Opponents 1-7: the race's computer cars in car slot order.

    // Registers the hidden options (npc_cars.json). Call before recompui::config::finalize().
    void init_config();
    // Loads the saved choices. Call after recompui::config::finalize().
    void load_config();
    // Adds the collapsible AI Opponents section to the Players tab's list.
    void add_section(recompui::Element* parent);
}

#endif
