#ifndef __RUSH2_H__
#define __RUSH2_H__

#include <cstdint>
#include <string>

namespace rush2 {
    inline const std::u8string program_id = u8"Rush2Recompiled";
    inline const std::string program_name = "Rush 2: Recompiled";

    void register_overlays();

    // Creates the config menu tabs. Must be called before recomp::start.
    void init_config();

    // Forces every model to its most detailed LOD and turns off LOD distance culling (src/lod.cpp).
    void set_lod_disabled(bool disabled);

    // Per-port controller input (src/input.cpp). Each controller claims a port with its first button press.
    namespace input {
        void poll();
        bool is_port_connected(int port);
        bool get_n64_input(int port, uint16_t* buttons, float* x, float* y);
        void set_rumble(int port, bool on);
        void update_rumble();
    }
}

#endif
