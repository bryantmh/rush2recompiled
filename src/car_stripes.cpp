// Rush 2's stripes on one-body cars (include/car_stripes.h, docs/car_mods.md#stripes-on-one-body-cars).

#include <cmath>
#include <cstring>

#include "car_stripes.h"

namespace rush2::car_stripes {

namespace {
    // Each STRIPE value's tile across half the car, from the centerline out, in 64ths: its alpha nibble (0-F). Read
    // from the 64x32 _L2 tiles of asset 0x1C (1ST_, 1F_, 1FD_, 2ST_, 2F_, 3ST_, 3A_: the prefix table 0x800C6518
    // indexed by the STRIPE value); the 32-wide and other tiles have the same bands at their own size.
    const char* const profiles[values] = {
        "",
        "ffffffffff",                   // 1 SINGLE
        "ffffffffffffffffffff",         // 2 SINGLE FAT
        "ffffccbb99775522",             // 3 SNGL FADE
        "000000ffffffffff",             // 4 DOUBLE
        "00000000ffffffffffffffffffff", // 5 DOUBLE FAT
        "ffffffffff0000ffffff",         // 6 TRIPLE A
        "ff0000ffffffffffff",           // 7 TRIPLE B
    };
}

std::vector<Band> bands(int stripe) {
    std::vector<Band> out;
    if (stripe <= 0 || stripe >= values) return out;
    const char* p = profiles[stripe];
    size_t n = strlen(p);
    auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    for (size_t i = 0; i < n;) {
        int level = nibble(p[i]);
        size_t j = i;
        while (j < n && nibble(p[j]) == level) j++;
        if (level > 0) out.push_back({ float(i) / 64.0f, float(j) / 64.0f, level });
        i = j;
    }
    return out;
}

int level_slot(int level) {
    for (int i = 0; i < level_count; i++) {
        if (level_values[i] == level) return i;
    }
    return -1;
}

float outward_normal_y(const float a[3], const float b[3], const float c[3], const float center[3]) {
    float e1[3], e2[3], n[3], mid[3];
    for (int i = 0; i < 3; i++) {
        e1[i] = b[i] - a[i];
        e2[i] = c[i] - a[i];
        mid[i] = (a[i] + b[i] + c[i]) / 3.0f - (center != nullptr ? center[i] : 0.0f);
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len <= 0.0f) return 0.0f;
    float out = n[0] * mid[0] + n[1] * mid[1] + n[2] * mid[2];
    return (out < 0.0f ? -n[1] : n[1]) / len;
}

}
