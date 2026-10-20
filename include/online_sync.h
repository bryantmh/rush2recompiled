#ifndef __ONLINE_SYNC_H__
#define __ONLINE_SYNC_H__

// Race state the port's C++ keeps outside RDRAM (2049 wings, California Speed's physics, the 2049 movers, battles...),
// for online play: the machines compare it with the race's RDRAM state every 30 frames and a resync copies the host's
// (docs/online.md#resyncs), so state kept here can't leave a machine's race behind.
//
// Each module lists its race state once, field by field, in a function taking a Walker (its online_sync): the same
// walk saves the fields (for the hash and the host's snapshot) and loads them back (a resync). Only what the race
// runs on belongs there: not what one machine draws or plays for its own players (views, sounds, HUD), which differ by
// machine and would make the races look different when they aren't.

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace rush2::online::sync {
    class Walker {
    public:
        virtual ~Walker() = default;
        // Saving appends the bytes; loading overwrites them.
        virtual void bytes(void* data, size_t size) = 0;
        // A container's element count: saving writes `n`, loading reads the host's into it.
        virtual void count(size_t& n) = 0;
        virtual bool loading() const = 0;
        // RDRAM the module's state lives in (the walk reads or writes these words in place).
        uint8_t* rdram = nullptr;
    };

    template <class T>
    void field(Walker& w, T& v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "walk a struct's fields one by one");
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t b = v ? 1 : 0;
            w.bytes(&b, 1);
            v = b != 0;
        }
        else {
            w.bytes(&v, sizeof(v));
        }
    }

    template <class T, size_t N>
    void field(Walker& w, T (&a)[N]) {
        for (T& v : a) {
            field(w, v);
        }
    }

    template <class T, size_t N>
    void field(Walker& w, std::array<T, N>& a) {
        for (T& v : a) {
            field(w, v);
        }
    }

    inline void field(Walker& w, std::string& s) {
        size_t n = s.size();
        w.count(n);
        s.resize(n);
        if (n != 0) {
            w.bytes(s.data(), n);
        }
    }

    // A random engine's whole state, as its stream form.
    template <class Engine>
    void engine(Walker& w, Engine& e) {
        std::string text;
        if (!w.loading()) {
            std::ostringstream out;
            out << e;
            text = out.str();
        }
        field(w, text);
        if (w.loading()) {
            std::istringstream in(text);
            in >> e;
        }
    }

    // A vector of elements `each(w, element)` walks; loading resizes it to the host's count.
    template <class T, class F>
    void vector(Walker& w, std::vector<T>& v, F each) {
        size_t n = v.size();
        w.count(n);
        v.resize(n);
        for (T& e : v) {
            each(w, e);
        }
    }

    template <class T>
    void vector(Walker& w, std::vector<T>& v) {
        vector(w, v, [](Walker& w, T& e) { field(w, e); });
    }

    // RDRAM words in place (an aligned range of whole words).
    void rdram_words(Walker& w, uint32_t address, uint32_t size);
}

// The modules' walks (online_game.cpp runs them in this order; each takes its own lock).
namespace rush2::wings { void online_sync(rush2::online::sync::Walker& w); }
namespace rush2::calspeed::physics { void online_sync(rush2::online::sync::Walker& w); }
namespace rush2::track1 { void buses_online_sync(rush2::online::sync::Walker& w); }
namespace rush2::track2049 { void movers_online_sync(rush2::online::sync::Walker& w); }
namespace rush2::stunt2049 { void online_sync(rush2::online::sync::Walker& w); }
namespace rush2::battle { void online_sync(rush2::online::sync::Walker& w); }
namespace rush2::battle_ai { void online_sync(rush2::online::sync::Walker& w); }

#endif
