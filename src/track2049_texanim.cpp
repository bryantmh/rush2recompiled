// Rush 2049's animated track textures in Rush 2 (spec: docs/rush2049_research/texanim.md).
//
// Rush 2049 animates track textures by rewriting the texture-load lists of its loaded track geometry once per frame
// (func_800BD2C8, set up by func_800BDAA8 from two per-track tables in its main data):
// - Flip-books (blinking barriers, chasing lights, the WARN90 sign): every `period` seconds a target texture's load
//   list gets the next frame's texel address in its first G_SETTIMG.
// - Scrolls (water, arrows, magma, vapours, the green flame): the tile origin (uls or ult) of every G_SETTILESIZE in
//   the target's load list follows a position that advances a fixed amount per vblank.
// Rush 2 draws the converted geometry's load lists the same way (models call them with G_DL, including the mirrored
// and TLUT-restoring list copies), so patching the same words in the loaded file animates every use. The converter
// records the patch sites (ConvertedTrack::tex_anims); this file finds where the host slot's geometry was loaded
// and runs 2049's update on it, as 2049 runs it at 30 frames per second.
//
// Differences from 2049, none visible: 2049 skips the animation in multiplayer without the Expansion Pak (this runs
// it, as 2049 does with the pak), and its scroll positions carry over from the previous race (here each race starts
// from the table's position).

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "recomp.h"
#include "track2049.h"

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t slot_records = 0x8010C258;   // 12 bytes each: u8 asset, u8 model slot, u8 mirror, ptr data.
    constexpr uint32_t slot_record_size = 12;
    constexpr int slot_record_count = 64;
    constexpr uint32_t geometry_record = 0x8010C400; // s32: the track geometry's slot record (func_800A4C98).
    constexpr int geometry_asset = 0x33;              // + track id

    float from_bits(uint32_t bits) {
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    // 2049's timing: 0x8002AFB8 = seconds per vblank; a frame lasts 0x8002EB98 vblanks (its frame time 0x8002EB94 is
    // the product) and is taken as 2 (30 fps). Scroll timers count down from 0x80123E58 and rearm with rate times
    // 0x80123E28.
    const float vblank_time = from_bits(0x3C888889);
    constexpr int frame_vblanks = 2;
    const float frame_time = (float)frame_vblanks * vblank_time;
    const float scroll_timer_start = from_bits(0x3D088880);
    const float scroll_interval = from_bits(0x3D088880);
    constexpr int max_catch_up = 8;    // 2049 frames run per tick at most

    struct FlipState {
        int16_t current;
        float timer;
    };

    struct ScrollState {
        int16_t position;
        float timer;
    };

    std::mutex texanim_mutex;
    rush2::track2049::TexAnims anims;
    std::vector<FlipState> flip_states;
    std::vector<ScrollState> scroll_states;
    bool setup_pending = false;
    bool active = false;
    uint32_t base = 0;       // Load address of the converted geometry.
    float pending_time = 0;  // Time not yet run as 2049 frames.

    uint32_t read32(uint8_t* rdram, uint32_t addr) {
        return (uint32_t)MEM_W(0, (int32_t)addr);
    }

    void write32(uint8_t* rdram, uint32_t addr, uint32_t value) {
        MEM_W(0, (int32_t)addr) = (int32_t)value;
    }

    // A geometry-relative G_SETTIMG w1 as Rush 2's loader rebases it (func_80077B38).
    uint32_t rebase(uint32_t w1) {
        return ((w1 + base) & 0xFFFFFF) | (w1 & 0x0F000000);
    }

    // The load address of the host slot's geometry (asset 0x33 + slot), or 0.
    uint32_t find_geometry(uint8_t* rdram) {
        int asset = geometry_asset + rush2::track2049::host_slot;
        int32_t index = (int32_t)read32(rdram, geometry_record);
        if (index >= 0 && index < slot_record_count) {
            uint32_t r = slot_records + (uint32_t)index * slot_record_size;
            if ((read32(rdram, r) >> 24) == (uint32_t)asset) {
                return read32(rdram, r + 4);
            }
        }
        for (int i = 0; i < slot_record_count; i++) {
            uint32_t r = slot_records + (uint32_t)i * slot_record_size;
            if ((read32(rdram, r) >> 24) == (uint32_t)asset) {
                return read32(rdram, r + 4);
            }
        }
        return 0;
    }

    // Finds the loaded geometry and checks that every patch site holds what the conversion put there (any frame's
    // texels for a flip-book's G_SETTIMG, the tile size for a scroll's G_SETTILESIZE).
    bool setup(uint8_t* rdram) {
        base = find_geometry(rdram);
        if (base < 0x80000000 || base >= 0x80800000) {
            printf("[2049] Animated textures: the track geometry isn't loaded\n");
            return false;
        }
        for (const auto& f : anims.flipbooks) {
            uint32_t w0 = read32(rdram, base + f.settimg), w1 = read32(rdram, base + f.settimg + 4);
            bool ok = (w0 >> 24) == 0xFD;
            bool frame = false;
            for (uint32_t v : f.frames) {
                frame |= w1 == rebase(v);
            }
            if (!ok || !frame) {
                printf("[2049] Animated textures: %s's load list isn't at 0x%X; animation off\n", f.target.c_str(),
                       base + f.settimg);
                return false;
            }
        }
        for (const auto& s : anims.scrolls) {
            for (const auto& c : s.tile_sizes) {
                if ((read32(rdram, base + c.offset) >> 24) != 0xF2 || read32(rdram, base + c.offset + 4) != c.w1) {
                    printf("[2049] Animated textures: %s's load list isn't at 0x%X; animation off\n",
                           s.target.c_str(), base + c.offset);
                    return false;
                }
            }
        }
        flip_states.clear();
        for (const auto& f : anims.flipbooks) {
            flip_states.push_back({ f.start, f.period });
        }
        scroll_states.clear();
        for (const auto& s : anims.scrolls) {
            scroll_states.push_back({ s.position, scroll_timer_start });
        }
        pending_time = 0;
        return true;
    }

    // func_800BD104: sets the tile origin s (or t) of every G_SETTILESIZE of a load list, halving it for each tile
    // smaller than the first (the mipmap levels).
    void set_tile_origin(uint8_t* rdram, const rush2::track2049::TexScroll& s, int value) {
        int size = -1;
        for (const auto& c : s.tile_sizes) {
            uint32_t addr = base + c.offset;
            uint32_t w0 = read32(rdram, addr);
            int extent = (int)(s.t ? (c.w1 & 0xFFF) : ((c.w1 >> 12) & 0xFFF)) + 4;
            if (size < 0) {
                size = extent;
            }
            else if (extent < size) {
                do {
                    size >>= 1;
                    value >>= 1;
                } while (extent < size);
            }
            if (s.t) {
                w0 = (w0 & 0xFFFFF000) | ((uint32_t)value & 0xFFF);
            }
            else {
                w0 = (w0 & 0xFF000FFF) | (((uint32_t)value & 0xFFF) << 12);
            }
            write32(rdram, addr, w0);
        }
    }

    // One 2049 frame of func_800BD2C8: the scrolls, then the flip-books.
    void run_frame(uint8_t* rdram) {
        for (size_t i = 0; i < anims.scrolls.size(); i++) {
            const auto& s = anims.scrolls[i];
            ScrollState& st = scroll_states[i];
            st.timer = st.timer - frame_time;
            if (0.0f < st.timer) {
                continue;
            }
            st.timer = st.timer + (float)s.rate * scroll_interval;
            st.position = (int16_t)(st.position + s.speed * frame_vblanks);
            if (st.position < 0) {
                st.position = (int16_t)(st.position + s.wrap);
            }
            else if (st.position >= s.wrap) {
                st.position = (int16_t)(st.position - s.wrap);
            }
            set_tile_origin(rdram, s, st.position >> 2);
        }
        for (size_t i = 0; i < anims.flipbooks.size(); i++) {
            const auto& f = anims.flipbooks[i];
            FlipState& st = flip_states[i];
            st.timer = st.timer - frame_time;
            if (!(st.timer <= 0.0f)) {
                continue;
            }
            st.timer = f.period;
            int count = (int)f.frames.size();
            if (f.forward) {
                st.current = (int16_t)(st.current + 1 >= count ? 0 : st.current + 1);
            }
            else {
                st.current = (int16_t)(st.current - 1 < 0 ? count - 1 : st.current - 1);
            }
            write32(rdram, base + f.settimg + 4, rebase(f.frames[st.current]));
        }
    }
}

void rush2::track2049::set_texanim_data(const TexAnims& data) {
    std::lock_guard lock{ texanim_mutex };
    anims = data;
    setup_pending = true;
    active = false;
}

void rush2::track2049::texanim_reset() {
    std::lock_guard lock{ texanim_mutex };
    setup_pending = true;
    active = false;
}

void rush2::track2049::texanim_tick(uint8_t* rdram, float dt) {
    std::lock_guard lock{ texanim_mutex };
    if (race_track() == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        active = false;
        return;
    }
    if (setup_pending) {
        setup_pending = false;
        active = (!anims.flipbooks.empty() || !anims.scrolls.empty()) && setup(rdram);
    }
    if (!active || !(dt > 0.0f)) {
        return;
    }
    pending_time += dt;
    for (int n = 0; pending_time >= frame_time; n++) {
        if (n == max_catch_up) {
            pending_time = 0;
            break;
        }
        pending_time -= frame_time;
        run_frame(rdram);
    }
}
