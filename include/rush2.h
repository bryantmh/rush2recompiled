#ifndef __RUSH2_H__
#define __RUSH2_H__

#include <string>

namespace rush2 {
    inline const std::u8string program_id = u8"Rush2Recompiled";
    inline const std::string program_name = "Rush 2: Recompiled";

    void register_overlays();

    // Creates the config menu tabs. Must be called before recomp::start.
    void init_config();
}

#endif
