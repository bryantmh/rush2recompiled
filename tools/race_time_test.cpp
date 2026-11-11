// The race time model (src/race_time_model.cpp) outside the game: the lap time Rush 2's average car needs on a
// converted track's lane 0, dry and with a [handling] traction (the PS1 SF Rush's snow), and their ratio. Gives the
// slowdown a slippery track's physics alone causes, e.g. what share of their night pace snow drones should run
// (src/sfrush_ps1/ps1_track.cpp snow_tuning). Built and run by tools/race_time_test.bat:
//
//   race_time_test.exe PATH.bin [PATH.bin ...] [--traction MPH,FACTOR[,MPH,FACTOR...]]   (default 32.1,0.5)
//
// PATH.bin: a converter output's path.bin (tools/track_mod_test.bat). Prints per file the lap (last profile entry, s),
// dry and with the traction, and snow / dry.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "race_time_model.h"

// The game's car speed mode (car2049.cpp) isn't part of a lap time; apply() alone reads it.
namespace rush2::car2049 { float rush2_lane_scale() { return 1.0f; } }

int main(int argc, char** argv) {
    std::vector<std::array<float, 2>> traction = { { 32.1f, 0.5f } };
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--traction" && i + 1 < argc) {
            traction.clear();
            std::string v = argv[++i];
            std::vector<float> nums;
            size_t o = 0;
            while (o < v.size()) {
                size_t e = v.find(',', o);
                nums.push_back(std::strtof(v.substr(o, e == std::string::npos ? std::string::npos : e - o).c_str(), nullptr));
                if (e == std::string::npos) break;
                o = e + 1;
            }
            for (size_t k = 0; k + 1 < nums.size(); k += 2) traction.push_back({ nums[k], nums[k + 1] });
        }
        else files.push_back(a);
    }
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<float> dry = rush2::race_time::lane_profile(data), slip = rush2::race_time::lane_profile(data, traction);
        if (dry.empty() || slip.empty()) {
            printf("%s: unreadable path\n", f.c_str());
            continue;
        }
        printf("%s: lap %.1f s dry, %.1f s with the traction; pace ratio %.3f\n", f.c_str(), dry.back(), slip.back(),
               dry.back() / slip.back());
    }
    return 0;
}
