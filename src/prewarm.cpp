// Background builds (include/prewarm.h, docs/prewarm.md).
//
// Everything the game converts from the other games' ROMs and from mods is built here off the game thread, into the
// disk cache (src/track_cache.cpp), as soon as the launcher starts:
// - First the cars, which the boot installs (rush2_heap_init): the Rush 2049 cars and part models, California Speed's
//   cars and name logos, and the car mods' plan (rush2::car_mods::prepare), each on a thread of its own so they are
//   done, or nearly, when the boot gets to them. They are read into memory too, so the boot only installs them. The
//   boot waits on a cache entry's claim for anything still being made rather than making it again.
// - Then every track: SF Rush's, Rush 2049's (races and arenas), California Speed's (as its scenery options are set)
//   and the enabled mods' custom tracks, at below-normal priority on two threads. A track the disk holds for this build
//   is skipped by its key alone.
// - Then, about once a second, whatever changed: another Rush 2049 source or California Speed ROM (its cars, for the
//   menus' check_source / check_cars to install), an option turned on, other scenery settings, other enabled mods.
//
// At the car select (frame), the chosen track's entry is read into memory for the race about to load it, so the race
// start reads nothing from the disk.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "librecomp/game.hpp"
#include "recomp.h"

#include "calspeed.h"
#include "car2049.h"
#include "car_mods.h"
#include "prewarm.h"
#include "rush2049_rom.h"
#include "track1.h"
#include "track2049.h"
#include "track_mod_runtime.h"
#include "wings.h"

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;    // s32: 2 the car select
    constexpr uint32_t track_id = 0x8010C3F0;      // s8: the track select's id until the race setup (enter_race)

    void name_thread(const wchar_t* name, bool low) {
#ifdef _WIN32
        SetThreadDescription(GetCurrentThread(), name);
        if (low) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
        (void)name;
        if (low) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    }

    // The game has started and recomp::get_rom (Rush 2's own assets, rush2::assets::rom_original) is in its final
    // buffer: the boot grows the image once (rush2::track1::rom_image_final), freeing the one loaded first.
    void wait_for_rush2_rom() {
        while (!rush2::track1::rom_image_final()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // What the tracks are built from: the ROMs, their options and settings, the enabled mods.
    std::string tracks_signature() {
        auto r49 = rush2::wings::get_rom();
        auto cs = rush2::calspeed::get_rom();
        auto r1 = rush2::track1::get_rom();
        char s[160];
        snprintf(s, sizeof(s), "%p%d %p%d%d%d %p%d ", (const void*)r49.get(), (int)rush2::track2049::available(),
                 (const void*)cs.get(), (int)rush2::calspeed::available(), (int)rush2::calspeed::scenery_enabled(),
                 (int)rush2::calspeed::billboards_enabled(), (const void*)r1.get(), (int)rush2::track1::available());
        return s + rush2::track_mod::signature();
    }

    void prepare_tracks() {
        std::thread other([] {
            name_thread(L"Prewarm Tracks", true);
            if (auto r1 = rush2::track1::get_rom(); r1 != nullptr && rush2::track1::available()) rush2::track1::prepare(*r1);
            rush2::track_mod::prepare();
        });
        rush2::track2049::prepare();
        other.join();
    }

    void run() {
        name_thread(L"Prewarm", false);
        auto r49 = rush2::wings::get_rom();
        auto cs = rush2::calspeed::get_rom();
        {
            std::vector<std::thread> cars;
            if (r49 != nullptr) cars.emplace_back([r49] { name_thread(L"Prewarm 2049 Cars", false); rush2::car2049::prepare(*r49); });
            if (cs != nullptr) cars.emplace_back([cs] { name_thread(L"Prewarm CS Cars", false); rush2::calspeed::prepare_cars(*cs); });
            cars.emplace_back([] {
                name_thread(L"Prewarm Car Mods", false);
                wait_for_rush2_rom();
                rush2::car_mods::prepare();
            });
            for (std::thread& t : cars) t.join();
        }
        printf("[prewarm] cars ready\n");
        fflush(stdout);
        name_thread(L"Prewarm", true);
        wait_for_rush2_rom();
        auto started = std::chrono::steady_clock::now();
        std::string built = tracks_signature();
        prepare_tracks();
        printf("[prewarm] tracks ready (%.1f s)\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        fflush(stdout);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (auto now = rush2::wings::get_rom(); now != r49) {
                r49 = now;
                if (r49 != nullptr) rush2::car2049::prepare(*r49);
            }
            if (auto now = rush2::calspeed::get_rom(); now != cs) {
                cs = now;
                if (cs != nullptr) rush2::calspeed::prepare_cars(*cs);
            }
            std::string now = tracks_signature();
            if (now != built) {
                built = now;
                prepare_tracks();
            }
        }
    }
}

void rush2::prewarm::start() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(run).detach();
}

void rush2::prewarm::frame(uint8_t* rdram) {
    static int preloaded = -1;
    if (MEM_W(0, (int32_t)game_state) != 2) {
        preloaded = -1;
        return;
    }
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (t == preloaded) return;
    preloaded = t;
    std::thread([t] {
        name_thread(L"Prewarm Race", false);
        if (t >= rush2::track1::first_menu_id && t < rush2::track1::first_menu_id + rush2::track1::track_count) {
            rush2::track1::preload(t - rush2::track1::first_menu_id + 1);
        }
        else if (t >= rush2::track2049::mod_menu_id && t < rush2::track2049::mod_menu_id + rush2::track2049::mod_capacity) {
            rush2::track_mod::preload(t - rush2::track2049::mod_menu_id);
        }
        else {
            rush2::track2049::preload(t);
        }
    }).detach();
}
