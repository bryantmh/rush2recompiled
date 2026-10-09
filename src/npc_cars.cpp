// The computer cars' car, paint, stripe and rims (the Players tab's AI Opponents section), as the GameShark codes for
// the opponents' cars did (800D9C1A + 9n: the car table's type byte; 800F6476 + 0x81C n: the physics car's copy).
//
// func_800A37F4 (race_preload_car_assets) draws each drone slot's car table entry (0x800D9C10 + slot * 9): +1 type
// (0-15, a type no car before it has), +2 MAIN COLOR (0-31), then either no stripe (+3 ACCENT COLOR another color,
// +4 STRIPE 0) or a stripe (+3 = MAIN, +4 STRIPE 1-7, +5 STRIPE COLOR another color). func_8009E6DC copies them to the
// physics car (+0x7EA type, +0x7EB..+0x7EE). Opponent n is the n-th drone slot (physics car +0x7E8 == 1) in slot
// order, so with one player it is car slot n. The choices are applied where the drone's colors have been drawn
// (hook at 0x800A3E80, where the stripe and no-stripe paths join, ahead of the ghost races' own, which take their
// slots back; the hook then reloads $v1, the type): the type set there skips the draw's check against the other
// cars, and the asset fit check after it still applies. Nothing is applied in attract
// mode (0x800E7BB0) or when the New York Cabs cheat (0x800D9E89) makes every drone a taxi on its tracks, which wins.
//
// TIRE RIMS aren't in the entry: a car's rim is the per-type table 0x80201020 (Rush 2's 0x800C0E48, src/rush2049/car2049.cpp)
// at row 0 for a drone (row player + 1 for a human), RIM01 + value. It is read in four places, each hooked to give an
// opponent its chosen rim: func_80085FD0 building a race car's wheel nodes (0x800865CC, for the car
// func_80086CA4 is building, 0x80086E0C), func_8005A598's per-frame wheel texture (sharp at 0x8005AB08, blurred
// through 0x800CEDAC at 0x8005AAF8), func_80087290 (0x800875F8) and func_8008DBA0's car init (+0x59E, 0x8008DD54).

// librecomp's own nlohmann::json first, so Config::load_config links (see src/rush2049/wings.cpp).
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_slider.h"

#include "car2049.h"
#include "npc_cars.h"
#include "options_page.h"
#include "rush2_hooks.h"

namespace {
    using rush2::npc_cars::count;

    constexpr uint32_t cars = 0x800F5470;           // physics cars, 0x81C each
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_kind = 0x7E8;            // u8: 1 drone, 2 human
    constexpr uint32_t attract_mode = 0x800E7BB0;   // u8
    constexpr uint32_t cabs_cheat = 0x800D9E89;     // u8: New York Cabs
    constexpr uint32_t cabs_track_select = 0x8010C3F0; // s8: the cheat makes taxis when this is 1 or 3
    constexpr int max_cars = 8;

    constexpr int rush2_types = 22;                 // 22 is Rush 2's "no car"
    constexpr int colors = 32;                      // 0x800CE19C
    constexpr int stripes = 8;                      // 0 none, 1-7

    // The car select's STRIPE values (strings 0x800CC8D8.., through the value table 0x800C4960; "SNGL FADE" spelled
    // out). Its TIRE RIMS values are "RIM 1".."RIM 21" (0x800C4980..).
    const char* const stripe_names[stripes] = {
        "None", "Single", "Single Fat", "Single Fade", "Double", "Double Fat", "Triple A", "Triple B",
    };
    constexpr int rims = 21;                        // RIM01-RIM21

    // Rush 2's paint colors, 0x800CE19C (RGBA), as the car select's swatches show them.
    constexpr uint32_t paint_rgba[colors] = {
        0xF0F0F0FF, 0xC0C0C0FF, 0x303030FF, 0x000000FF, 0xF060F0FF, 0xF00000FF, 0x800000FF, 0x400000FF,
        0x100000FF, 0x8080F0FF, 0x0000F0FF, 0x000080FF, 0x000040FF, 0x000010FF, 0x9ADC0FFF, 0x00C800FF,
        0x008000FF, 0x003000FF, 0x00E0BCFF, 0x00986CFF, 0x004B39FF, 0xF000F0FF, 0xA088F0FF, 0x9000F0FF,
        0x340073FF, 0x34005DFF, 0xF0F000FF, 0xE6B430FF, 0xF08000FF, 0x984010FF, 0xFFECCAFF, 0x5D3229FF,
    };

    // Rush 2's car types 0-21 as the car select names them: its name logos (asset 0x11), looked up through the
    // overlay's type -> name table 0x803C7688 (PICKUP, COMPACT, MUSCLE, ...; DEW is the Mountain Dew car). The part
    // name prefixes (0x800CAF3C: PICKUP, INTEG, VETTE, SLED, ...) are the licensed models they were built from.
    const char* const rush2_names[rush2_types] = {
        "Pickup", "Compact", "Muscle", "Mobster", "Sedan", "Bandit", "Coupe", "Exotic", "Van", "Sportster",
        "Subcompact", "Concept", "Hatchback", "Cruiser", "Stallion", "4x4", "Taxi", "Hot Rod", "Formula", "Prototype",
        "Rocket", "Mountain Dew",
    };

    enum Field { f_car, f_stripe, f_rims, f_main, f_accent, f_stripe_color, field_count };
    const char* const field_ids[field_count] = { "car", "stripe", "rims", "main_color", "accent_color", "stripe_color" };
    constexpr int field_max[field_count] = { rush2::car2049::types - 1, stripes - 1, rims - 1, colors - 1, colors - 1,
                                             colors - 1 };
    constexpr int random_value = -1; // Random (the rims: the car's own)

    const std::string config_id = "npc_cars";
    recomp::config::Config npc_config{ "AI Opponents", config_id, false };

    // The choices, read by the game thread.
    std::array<std::array<std::atomic<int>, field_count>, count> choices;

    // Game thread: each car slot's chosen rim (or random_value), set as the race's drones are drawn.
    std::array<int, max_cars> slot_rims;
    int building_car = -1; // The car func_80086CA4 is building wheel nodes for.
    std::minstd_rand rng{ (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() };

    std::string option_id(int npc, Field f) {
        return "npc" + std::to_string(npc + 1) + "_" + field_ids[f];
    }

    void set_choice(int npc, Field f, int value) {
        choices[npc][f].store(value);
        npc_config.update_option_value(option_id(npc, f), (double)value);
        npc_config.save_config();
    }

    // Every opponent back to Random (rims: the car's own).
    void reset_choices() {
        for (int npc = 0; npc < count; npc++) {
            for (int f = 0; f < field_count; f++) {
                choices[npc][f].store(random_value);
                npc_config.update_option_value(option_id(npc, (Field)f), (double)random_value);
            }
        }
        npc_config.save_config();
    }

    // A physics car's index from its address, or -1.
    int car_index(uint32_t car) {
        if (car < cars || car >= cars + max_cars * car_size || (car - cars) % car_size != 0) {
            return -1;
        }
        return (int)((car - cars) / car_size);
    }

    int rim_of(uint8_t* rdram, int car) {
        if (car < 0 || car >= max_cars || MEM_BU(0, (int32_t)(cars + car * car_size + car_kind)) != 1) {
            return random_value;
        }
        return slot_rims[car];
    }

    bool type_allowed(int type) {
        return (type >= 0 && type < rush2_types) ||
            (type >= rush2::car2049::first_type && type < rush2::car2049::types && rush2::car2049::available());
    }

    int other_color(int not_this) {
        int c = (int)(rng() % (colors - 1));
        return c >= not_this ? c + 1 : c;
    }
}

// func_800A37F4 at 0x800A3E80: drone slot $s2's colors have been drawn ($s1 = its car table entry).
extern "C" void rush2_npc_cars_apply(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int32_t)ctx->r18;
    uint32_t entry = (uint32_t)ctx->r17;
    if (slot < 0 || slot >= max_cars) {
        return;
    }
    slot_rims[slot] = random_value;
    if (MEM_BU(0, (int32_t)attract_mode) != 0 || MEM_BU(0, (int32_t)(cars + slot * car_size + car_kind)) != 1) {
        return;
    }
    int8_t cabs_select = MEM_B(0, (int32_t)cabs_track_select);
    if (MEM_BU(0, (int32_t)cabs_cheat) != 0 && (cabs_select == 1 || cabs_select == 3)) {
        return;
    }
    int npc = 0;
    for (int j = 0; j < slot; j++) {
        npc += MEM_BU(0, (int32_t)(cars + j * car_size + car_kind)) == 1 ? 1 : 0;
    }
    if (npc >= count) {
        return;
    }
    auto choice = [npc](Field f) { return choices[npc][f].load(); };

    int type = choice(f_car);
    if (type_allowed(type)) {
        MEM_B(1, (int32_t)entry) = (int8_t)type;
    }
    bool had_stripe = MEM_BU(4, (int32_t)entry) != 0;
    int main = choice(f_main);
    if (main != random_value) {
        // With a stripe the game paints the accent as the main color.
        if (had_stripe && choice(f_accent) == random_value) {
            MEM_B(3, (int32_t)entry) = (int8_t)main;
        }
        MEM_B(2, (int32_t)entry) = (int8_t)main;
    }
    main = MEM_BU(2, (int32_t)entry);
    if (choice(f_accent) != random_value) {
        MEM_B(3, (int32_t)entry) = (int8_t)choice(f_accent);
    }
    int stripe = choice(f_stripe);
    if (stripe != random_value) {
        MEM_B(4, (int32_t)entry) = (int8_t)stripe;
        // The game only draws a stripe color for a car it gave a stripe.
        if (stripe != 0 && !had_stripe && choice(f_stripe_color) == random_value) {
            MEM_B(5, (int32_t)entry) = (int8_t)other_color(main);
        }
    }
    if (choice(f_stripe_color) != random_value) {
        MEM_B(5, (int32_t)entry) = (int8_t)choice(f_stripe_color);
    }
    slot_rims[slot] = choice(f_rims);
    bool any = false;
    for (int f = 0; f < field_count; f++) {
        any = any || choice((Field)f) != random_value;
    }
    if (any) {
        printf("[npc] Opponent %d (car %d): type %d, colors %d/%d, stripe %d color %d, rims %d\n", npc + 1, slot,
            MEM_BU(1, (int32_t)entry), MEM_BU(2, (int32_t)entry), MEM_BU(3, (int32_t)entry),
            MEM_BU(4, (int32_t)entry), MEM_BU(5, (int32_t)entry), slot_rims[slot]);
        std::fflush(stdout);
    }
}

// func_80086CA4 at 0x80086E0C: about to build car $fp's nodes (func_80085FD0).
extern "C" void rush2_npc_cars_building(uint8_t*, recomp_context* ctx) {
    building_car = (int32_t)ctx->r30;
}

// func_80085FD0 at 0x800865CC: $t9 = the rim of node set $fp (car $v1 in a race), from row $s5.
extern "C" void rush2_npc_cars_rim_nodes(uint8_t* rdram, recomp_context* ctx) {
    int car = (int32_t)ctx->r3;
    if ((int32_t)ctx->r21 == 0 && car == building_car) {
        int rim = rim_of(rdram, car);
        if (rim != random_value) {
            ctx->r25 = (uint64_t)(int64_t)rim;
        }
    }
    building_car = -1;
}

// func_8005A598 at 0x8005AAF8 and 0x8005AB08: $v0 = the rim of car $t1, from row $ra.
extern "C" void rush2_npc_cars_rim_frame(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r31 != 0) {
        return;
    }
    int rim = rim_of(rdram, car_index((uint32_t)ctx->r9));
    if (rim != random_value) {
        ctx->r2 = (uint64_t)(int64_t)rim;
    }
}

// func_80087290 at 0x800875F8: $t9 = the rim of car [$sp + 0x3C], from row $v0.
extern "C" void rush2_npc_cars_rim_state(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r2 != 0) {
        return;
    }
    int rim = rim_of(rdram, car_index((uint32_t)MEM_W(0x3C, (int32_t)ctx->r29)));
    if (rim != random_value) {
        ctx->r25 = (uint64_t)(int64_t)rim;
    }
}

// func_8008DBA0 at 0x8008DD54: $t8 = the rim of car $s2 (stored at +0x59E), from row $s1.
extern "C" void rush2_npc_cars_rim_init(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r17 != 0) {
        return;
    }
    int rim = rim_of(rdram, car_index((uint32_t)ctx->r18));
    if (rim != random_value) {
        ctx->r24 = (uint64_t)(int64_t)rim;
    }
}

void rush2::npc_cars::init_config() {
    slot_rims.fill(random_value);
    for (int npc = 0; npc < count; npc++) {
        for (int f = 0; f < field_count; f++) {
            choices[npc][f].store(random_value);
            npc_config.add_number_option(option_id(npc, (Field)f), field_ids[f], "", random_value, field_max[f], 1, 0,
                false, random_value, true);
        }
    }
}

void rush2::npc_cars::load_config() {
    npc_config.load_config();
    for (int npc = 0; npc < count; npc++) {
        for (int f = 0; f < field_count; f++) {
            int value = (int)std::get<double>(npc_config.get_option_value(option_id(npc, (Field)f)));
            choices[npc][f].store(value >= random_value && value <= field_max[f] ? value : random_value);
        }
    }
}

namespace {
    using namespace recompui;

    bool expanded = false; // UI thread: whether the section is open.

    recompui::Color paint_color(int color) {
        uint32_t c = paint_rgba[color];
        return { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), 255 };
    }

    // A line of the section: a name on the left, the control on the right.
    Element* add_line(Element* parent, const std::string& name) {
        ContextId context = get_current_context();
        Element* line = context.create_element<rush2::ui::FocusRow>(parent);
        line->set_display(Display::Flex);
        line->set_flex_direction(FlexDirection::Row);
        line->set_align_items(AlignItems::Center);
        line->set_gap(16.0f);
        line->set_as_navigation_container(NavigationType::Horizontal);
        Label* label = context.create_element<Label>(line, name, theme::Typography::LabelSM);
        label->set_color(theme::color::TextDim);
        label->set_width(170.0f);
        label->set_flex_shrink(0.0f);
        return line;
    }

    void add_select(Element* parent, const std::string& name, int npc, Field f,
                    const std::vector<std::pair<int, std::string>>& values) {
        ContextId context = get_current_context();
        Element* line = add_line(parent, name);
        std::vector<SelectOption> options;
        for (const auto& [value, text] : values) {
            options.emplace_back(text, std::to_string(value));
        }
        Select* select = context.create_element<Select>(line, options, std::to_string(choices[npc][f].load()));
        // Select is focusable but not in the tab order, which the d-pad's navigation (RmlUi's nav: auto) needs.
        select->set_tab_index_auto();
        select->add_change_callback([npc, f](SelectOption& option, int) {
            set_choice(npc, f, std::stoi(option.value));
        });
    }

    // A paint color: a slider from Random (0) through colors 1-32, and a swatch of the color (or "Random").
    void add_color(Element* parent, const std::string& name, int npc, Field f) {
        ContextId context = get_current_context();
        Element* line = add_line(parent, name);
        Element* swatch = context.create_element<Element>(line, 0, "div", false);
        swatch->set_width(32.0f);
        swatch->set_height(24.0f);
        swatch->set_flex_shrink(0.0f);
        swatch->set_border_width(1.0f);
        swatch->set_border_radius(4.0f);
        swatch->set_border_color(theme::color::WhiteA20);
        Label* random_label = context.create_element<Label>(line, "Random", theme::Typography::Body);
        random_label->set_width(70.0f);
        random_label->set_flex_shrink(0.0f);
        Slider* slider = context.create_element<Slider>(line, SliderType::Integer);
        slider->set_flex_grow(1.0f);
        slider->set_min_value(0);
        slider->set_max_value(colors);
        slider->set_step_value(1);
        auto show = [swatch, random_label](int color) {
            if (color == random_value) {
                swatch->set_background_color(theme::color::WhiteA5);
                random_label->set_text("Random");
            }
            else {
                swatch->set_background_color(paint_color(color));
                random_label->set_text("");
            }
        };
        int color = choices[npc][f].load();
        show(color);
        slider->set_value(color + 1);
        slider->add_value_changed_callback([npc, f, show](double value) {
            int color = (int)value - 1;
            set_choice(npc, f, color);
            show(color);
        });
    }

    void build_opponent(Element* parent, int npc) {
        ContextId context = get_current_context();
        Element* block = context.create_element<Element>(parent, 0, "div", false);
        block->set_display(Display::Flex);
        block->set_flex_direction(FlexDirection::Column);
        block->set_gap(6.0f);
        block->set_padding_top(8.0f);
        block->set_as_navigation_container(NavigationType::Vertical);
        context.create_element<Label>(block, "Opponent " + std::to_string(npc + 1), theme::Typography::LabelSM);

        std::vector<std::pair<int, std::string>> car_values = { { random_value, "Random" } };
        for (int t = 0; t < rush2_types; t++) {
            car_values.emplace_back(t, rush2_names[t]);
        }
        int chosen = choices[npc][f_car].load();
        for (int k = 0; k < rush2::car2049::car_count; k++) {
            int type = rush2::car2049::first_type + k;
            if (rush2::car2049::available() || chosen == type) {
                car_values.emplace_back(type, std::string(rush2::car2049::display_name(k)) + " (2049)");
            }
        }
        add_select(block, "Car", npc, f_car, car_values);

        std::vector<std::pair<int, std::string>> stripe_values = { { random_value, "Random" } };
        for (int s = 0; s < stripes; s++) {
            stripe_values.emplace_back(s, stripe_names[s]);
        }
        add_select(block, "Stripe", npc, f_stripe, stripe_values);

        std::vector<std::pair<int, std::string>> rim_values = { { random_value, "Car's Own" } };
        for (int r = 0; r < rims; r++) {
            rim_values.emplace_back(r, "Rim " + std::to_string(r + 1));
        }
        add_select(block, "Rims", npc, f_rims, rim_values);

        add_color(block, "Main Color", npc, f_main);
        add_color(block, "Accent Color", npc, f_accent);
        add_color(block, "Stripe Color", npc, f_stripe_color);
    }

    // The section: a heading with a button that opens and closes the opponents' choices below it.
    class Section : public Element {
    public:
        Section(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
            ContextId context = get_current_context();
            set_display(Display::Flex);
            set_flex_direction(FlexDirection::Column);
            set_padding(12.0f);
            set_gap(8.0f);
            set_width(100.0f, Unit::Percent);
            set_as_navigation_container(NavigationType::Vertical);

            Element* head = context.create_element<rush2::ui::FocusRow>(this);
            head->set_display(Display::Flex);
            head->set_flex_direction(FlexDirection::Row);
            head->set_align_items(AlignItems::Center);
            head->set_gap(16.0f);
            head->set_as_navigation_container(NavigationType::Horizontal);
            Element* titles = context.create_element<Element>(head, 0, "div", false);
            titles->set_display(Display::Flex);
            titles->set_flex_direction(FlexDirection::Column);
            titles->set_flex_grow(1.0f);
            titles->set_gap(4.0f);
            context.create_element<Label>(titles, "AI Opponents", theme::Typography::LabelMD);
            Label* note = context.create_element<Label>(titles,
                "The computer cars' car, stripe, rims and paint. Random keeps the game's pick.", theme::Typography::Body);
            note->set_color(theme::color::TextDim);
            Button* reset = context.create_element<Button>(head, "Reset", ButtonStyle::Secondary);
            reset->add_pressed_callback([this]() {
                reset_choices();
                stale = true;
                queue_update();
            });
            button = context.create_element<Button>(head, expanded ? "Hide" : "Show", ButtonStyle::Secondary);
            button->add_pressed_callback([this]() {
                expanded = !expanded;
                queue_update();
            });

            body = context.create_element<Element>(this, 0, "div", false);
            body->set_display(Display::Flex);
            body->set_flex_direction(FlexDirection::Column);
            body->set_as_navigation_container(NavigationType::Vertical);
            build();
        }

    protected:
        std::string_view get_type_name() override { return "NpcCarsSection"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update && (built != expanded || stale)) {
                button->set_text(expanded ? "Hide" : "Show");
                build();
            }
        }

    private:
        Button* button = nullptr;
        Element* body = nullptr;
        bool built = false;
        bool stale = false; // The choices changed under the shown controls (Reset).

        void build() {
            body->clear_children();
            stale = false;
            built = expanded;
            if (expanded) {
                for (int npc = 0; npc < count; npc++) {
                    build_opponent(body, npc);
                }
            }
        }
    };
}

void rush2::npc_cars::add_section(recompui::Element* parent) {
    recompui::get_current_context().create_element<Section>(parent);
}
