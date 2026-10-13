#ifndef __CIRCUIT_H__
#define __CIRCUIT_H__

#include <cstdint>
#include <functional>
#include <string>

namespace recompui {
    class Element;
}

// Circuit length and courses (src/circuit.cpp): the General tab's Circuit Length select and Circuit Courses section.
// A circuit takes both when it starts and keeps them (saves "circuits" section), so changing them only affects the
// next circuit.
namespace rush2::circuit {
    constexpr int stock_length = 28;    // Rush 2's circuit: 28 races.
    constexpr int course_ids = 25;      // Track select ids a circuit can pick from: Rush 2 0-6, 2049 12-17, SF Rush 18-24.

    // Registers the hidden choices (circuit.json). Call before recompui::config::finalize().
    void init_config();
    // Loads the saved choices. Call after recompui::config::finalize().
    void load_config();
    // Makes a callback that shows text in the page's description pane (OptionsPage::text_shower).
    using TextShower = std::function<std::function<void()>(const std::string&)>;
    // Adds the Circuit Length select and the collapsible Circuit Courses section to a list (the General tab's, under
    // Gameplay).
    void add_section(recompui::Element* parent, const TextShower& text_shower);

    // Game thread. The current circuit's (0x800D3E40's) length in races, and whether it may pick track t.
    int length(uint8_t* rdram);
    bool course_chosen(uint8_t* rdram, int t);

    // Accurate Circuit Review (General tab): the victory screen after a circuit shows its races, in order, each with
    // the car player 1 raced in it, racing or wrecked by the place there. Read when that screen starts.
    void set_accurate_review(bool enabled);
}

#endif
