#ifndef __DL_COPY_H__
#define __DL_COPY_H__

// Copies of the game's model display lists with some of their commands rewritten, for drawing a model differently
// without touching the loaded model (src/ghost.cpp: translucent ghost cars; src/rush2049/car2049.cpp: Rush 2049's car
// lighting on Rush 2's cars). A list is copied up to its G_ENDDL (or a G_DL branch), and the lists it calls (G_DL, and
// the 2049 conversion's conditional G_DL, E0 01) are copied the same way. Copies are kept by source address (and
// made again if the source list's commands changed) until reset(), which frees their space.

#include <cstdint>
#include <functional>
#include <map>

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
            uint32_t src = heap_list(dl);
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
            // A list loaded again at the same address (another model) is copied again.
            auto it = copies_.find(src);
            if (it != copies_.end() && it->second.hash == hash) return it->second.copy;
            uint32_t bytes = count * 8;
            if (cursor_ + bytes > end_) return 0;
            uint32_t copy = cursor_;
            cursor_ += bytes;
            copies_[src] = { copy, hash };
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
        struct Copy {
            uint32_t copy;
            uint64_t hash;    // of the source list's commands when it was copied
        };
        uint32_t start_, end_, cursor_;
        std::map<uint32_t, Copy> copies_;        // source list -> copy
    };
}

#endif
