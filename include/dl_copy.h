#ifndef __DL_COPY_H__
#define __DL_COPY_H__

// Copies of the game's model display lists with some of their commands rewritten, for drawing a model differently
// without touching the loaded model (src/ghost.cpp: translucent ghost cars; src/rush2049/car2049.cpp: Rush 2049's car
// lighting on Rush 2's cars). A list is copied up to its G_ENDDL (or a G_DL branch), and the lists it calls (G_DL, and
// the 2049 conversion's conditional G_DL, E0 01) are copied the same way. Copies are kept by source address and the
// state of its commands (a list whose commands changed gets another copy; up to four states a list) until reset(),
// which frees their space. A copy found again has its calls pointed at the current copies of the lists they reach,
// which change apart from it (issue 87: a mod car's brake light switch past a scheme branch stayed as first copied).

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "recomp.h"

namespace rush2::dl {
    // A display list address the G_DL commands of the game's models use (KSEG0 or physical) as KSEG0, if it is in
    // the game heap, where models are loaded; else 0.
    inline uint32_t heap_list(uint32_t addr) {
        uint32_t top = addr >> 24;
        if (top != 0x80 && top != 0x00) return 0;
        uint32_t a = 0x80000000u | (addr & 0xFFFFFF);
        return a >= 0x80400000 && a < 0x80B00000 ? a : 0;
    }

    class Copier {
    public:
        // Copies go in spare RDRAM [start, end) (below 16MB: display list addresses are 24 bits).
        Copier(uint32_t start, uint32_t end) : start_(start), end_(end), cursor_(start) {}

        // A copy of `dl` with `rewrite(w0, w1)` applied to each of its commands other than calls, or 0 if it isn't a
        // heap list or there's no room left.
        uint32_t copy(uint8_t* rdram, uint32_t dl, const std::function<void(uint32_t&, uint32_t&)>& rewrite,
                      int depth = 0) {
            uint32_t src = source(dl);
            if (src == 0 || depth > 8) return 0;
            uint32_t count = 0;
            uint64_t hash = 0xCBF29CE484222325ull;
            for (uint32_t o = src; count < 8192; o += 8) {
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)o), w1 = (uint32_t)MEM_W(4, (int32_t)o);
                hash = (hash ^ w0) * 0x100000001B3ull;
                hash = (hash ^ w1) * 0x100000001B3ull;
                count++;
                uint8_t op = w0 >> 24;
                if (op == 0xDF || (op == 0xDE && ((w0 >> 16) & 0xFF) == 1)) break;
            }
            // A list loaded again at the same address (another model), or whose commands the game switches (a mod car's
            // brake light switch, src/rush2049/car2049.cpp), is copied again; each state's copy is kept, so a switch
            // turned back finds its copy instead of filling the space.
            std::vector<Copy>& kept = copies_[src];
            for (const Copy& c : kept) {
                if (c.hash != hash) continue;
                const uint32_t found = c.copy;   // kept may change below (a list reached from itself)
                // The lists it calls or branches to are checked each time: they change apart from it (a scheme block
                // past a branch holds a brake switch, issue 87), and a stale call would draw them as first copied.
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(src + i * 8)), w1 = (uint32_t)MEM_W(4, (int32_t)(src + i * 8));
                    uint8_t op = w0 >> 24;
                    if (op != 0xDE && !(op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) continue;
                    uint32_t sub = this->copy(rdram, w1, rewrite, depth + 1);
                    uint32_t want = sub != 0 ? (w1 & 0xFF000000) | (sub & 0xFFFFFF) : w1;
                    if ((uint32_t)MEM_W(4, (int32_t)(found + i * 8)) != want) MEM_W(4, (int32_t)(found + i * 8)) = (int32_t)want;
                }
                return found;
            }
            uint32_t bytes = count * 8;
            if (cursor_ + bytes > end_) return 0;
            uint32_t copy = cursor_;
            cursor_ += bytes;
            if (kept.size() >= max_states) kept.erase(kept.begin());   // a model loaded over it: the old states are gone
            kept.push_back({ copy, hash });
            for (uint32_t i = 0; i < count; i++) {
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(src + i * 8)), w1 = (uint32_t)MEM_W(4, (int32_t)(src + i * 8));
                uint8_t op = w0 >> 24;
                if (op == 0xDE || (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) {
                    uint32_t sub = this->copy(rdram, w1, rewrite, depth + 1);
                    if (sub != 0) w1 = (w1 & 0xFF000000) | (sub & 0xFFFFFF);
                }
                else {
                    rewrite(w0, w1);
                }
                MEM_W(0, (int32_t)(copy + i * 8)) = (int32_t)w0;
                MEM_W(4, (int32_t)(copy + i * 8)) = (int32_t)w1;
            }
            return copy;
        }

        // Lists in [start, end) (KSEG0) may be copied too, besides the heap's: lists that stay where they are, such as
        // the Rush 2049 wing model (src/rush2049/wings_render.cpp).
        void allow(uint32_t start, uint32_t end) {
            extra_start_ = start;
            extra_end_ = end;
        }

        // A copy of `dl`, a list that isn't kept where it is (a side list built again each frame, such as the wings'
        // list around a car body), in `alloc(bytes)`: it isn't kept, but the lists it calls are copied (and kept) as
        // copy() does. 0 if it runs past `max_commands` without a G_ENDDL or alloc gives no room.
        uint32_t copy_once(uint8_t* rdram, uint32_t dl, const std::function<void(uint32_t&, uint32_t&)>& rewrite,
                           const std::function<uint32_t(uint32_t)>& alloc, uint32_t max_commands = 512) {
            uint32_t top = dl >> 24;
            if (top != 0x80 && top != 0x00) return 0;
            uint32_t src = 0x80000000u | (dl & 0xFFFFFF);
            uint32_t count = 0;
            for (uint32_t o = src;; o += 8) {
                if (count == max_commands) return 0;
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)o);
                count++;
                uint8_t op = w0 >> 24;
                if (op == 0xDF || (op == 0xDE && ((w0 >> 16) & 0xFF) == 1)) break;
            }
            uint32_t copy = alloc(count * 8);
            if (copy == 0) return 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(src + i * 8)), w1 = (uint32_t)MEM_W(4, (int32_t)(src + i * 8));
                uint8_t op = w0 >> 24;
                if (op == 0xDE || (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) {
                    uint32_t sub = this->copy(rdram, w1, rewrite);
                    if (sub != 0) w1 = (w1 & 0xFF000000) | (sub & 0xFFFFFF);
                }
                else {
                    rewrite(w0, w1);
                }
                MEM_W(0, (int32_t)(copy + i * 8)) = (int32_t)w0;
                MEM_W(4, (int32_t)(copy + i * 8)) = (int32_t)w1;
            }
            return copy;
        }

        // `bytes` (rounded up to 8) of the copies' space for other data the copies point at (src/car_probes.cpp: the
        // vertices a car's relit copies load), or 0 if there's no room left. Freed by reset() like the copies.
        uint32_t alloc(uint32_t bytes) {
            bytes = (bytes + 7) & ~7u;
            if (cursor_ + bytes > end_) return 0;
            uint32_t at = cursor_;
            cursor_ += bytes;
            return at;
        }

        void reset() {
            copies_.clear();
            cursor_ = start_;
        }

    private:
        // `addr` as KSEG0 if copy() may copy the list there (the heap or the allowed range), else 0.
        uint32_t source(uint32_t addr) const {
            uint32_t a = heap_list(addr);
            if (a != 0 || extra_end_ == 0) return a;
            uint32_t top = addr >> 24;
            if (top != 0x80 && top != 0x00) return 0;
            a = 0x80000000u | (addr & 0xFFFFFF);
            return a >= extra_start_ && a < extra_end_ ? a : 0;
        }

        struct Copy {
            uint32_t copy;
            uint64_t hash;    // of the source list's commands when it was copied
        };
        static constexpr size_t max_states = 4;  // copies kept per source list (lit and unlit brakes take two)
        uint32_t start_, end_, cursor_;
        uint32_t extra_start_ = 0, extra_end_ = 0;
        std::map<uint32_t, std::vector<Copy>> copies_;   // source list -> its copies, one per state of its commands
    };
}

#endif
