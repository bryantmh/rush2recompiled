// Builds (include/prewarm.h, docs/prewarm.md).
//
// Everything the game converts from the other games' ROMs and from mods is built here off the game thread, into the
// disk cache (src/track_cache.cpp):
// - At launch, before the game starts (build_pass, behind the launcher's progress screen), all at once: the arcade
//   mods rebuilt when out of date, then the car mods' plan and the custom tracks; the Rush 2049 cars and part models;
//   California Speed's cars and name logos; the Accurate Car Stats bars' ranges; SF Rush's tracks and car stripes;
//   Rush 2049's and California Speed's tracks; then the track select's art container. A track the disk holds for this build is skipped by its key alone, so a launch with
//   everything built takes a moment. The game starts when it's over, so the boot only installs and no menu or race
//   start waits on a build.
// - Then, about once a second in the background, whatever a change in the menus needs: another Rush 2049 source or
//   California Speed ROM (its cars, for the menus' check_source / check_cars to install), an option turned on, other
//   scenery settings, other enabled mods. The game waits on a cache entry's claim for anything still being made.
//
// At the car select (frame), the chosen track's entry is read into memory for the race about to load it, so the race
// start reads nothing from the disk.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
#include "car1_stripes.h"
#include "car2049.h"
#include "arcade2049.h"
#include "car_mods.h"
#include "prewarm.h"
#include "rush2049_rom.h"
#include "texture_prewarm.h"
#include "track1.h"
#include "track2049.h"
#include "track_cache.h"
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
        rush2::track2049::prepare_menu_container();   // the track select's art, from the tracks' menu art made above
    }

    // The launch's build pass (start), for the launcher's progress screen: each job with about how long it takes when
    // nothing is built yet (seconds, measured on a new build with the arcade mods out of date), so the bar moves at an
    // even pace: a job going counts for more of that time the longer it runs, never all of it (63% at its expected
    // time, 86% at twice that), so the bar keeps moving when a job takes longer; a finished one counts for the time it
    // took (a job that had everything built takes nothing off the bar).
    struct Job {
        const char* label;
        double seconds;
        bool done = false;
        double took = 0.0;
        std::optional<std::chrono::steady_clock::time_point> started;
    };
    std::mutex progress_mutex;
    std::vector<Job> jobs;
    std::atomic<bool> pass_done = false;

    // How long each job took the last time it had work to do on this computer (<config>/track_cache/launch_times.txt,
    // a "seconds label" line each), in place of the measured times above.
    std::map<std::string, double> learned;

    std::filesystem::path times_path() {
        return recomp::get_config_path() / "track_cache" / "launch_times.txt";
    }

    void load_times() {
        std::ifstream f(times_path());
        double seconds;
        std::string label;
        while (f >> seconds && std::getline(f >> std::ws, label)) {
            if (seconds > 0.0 && seconds < 600.0) learned[label] = seconds;
        }
    }

    void save_times() {
        std::error_code ec;
        std::filesystem::create_directories(times_path().parent_path(), ec);
        std::ofstream f(times_path(), std::ios::trunc);
        for (const auto& [label, seconds] : learned) f << seconds << ' ' << label << '\n';
    }

    void job(const char* label, const std::function<void()>& work) {
        {
            std::lock_guard lock{ progress_mutex };
            for (Job& j : jobs) if (std::string_view(j.label) == label) j.started = std::chrono::steady_clock::now();
        }
        work();
        std::lock_guard lock{ progress_mutex };
        for (Job& j : jobs) {
            if (std::string_view(j.label) != label) continue;
            j.done = true;
            j.took = std::chrono::duration<double>(std::chrono::steady_clock::now() - *j.started).count();
        }
    }

    // Every car and track built before the game starts, all at once on threads of their own. The Rush 2 ROM is loaded
    // for it here (recomp::load_stored_rom; the game loads it again when it starts, then the boot grows it, which is
    // why the background builds after the pass wait for rom_image_final). The arcade mods are rebuilt first when out
    // of date, and the car mods and custom tracks are built from what that installs.
    bool build_pass(std::u8string game_id) {
        if (!recomp::load_stored_rom(game_id)) return false;
        load_times();
        auto r49 = rush2::wings::get_rom();
        auto cs = rush2::calspeed::get_rom();
        auto r1 = rush2::track1::get_rom();
        std::vector<std::thread> threads;
        auto spawn = [&](const wchar_t* name, std::function<void()> work) {
            threads.emplace_back([name, work] {
                name_thread(name, false);
                work();
            });
        };
        const bool sf_rush = r1 != nullptr && rush2::track1::available();
        {
            std::lock_guard lock{ progress_mutex };
            jobs = { { "Rush 2049 arcade mods", 1.5 }, { "Car mods", 0.3 }, { "Custom tracks", 2.4 },
                     { "Rush 2049 and California Speed tracks", 3.3 }, { "Track select art", 0.2 },
                     { "Car stats", 1.5 } };
            if (r49 != nullptr) jobs.push_back({ "Rush 2049 cars", 1.0 });
            if (cs != nullptr) jobs.push_back({ "California Speed cars", 0.5 });
            if (sf_rush) jobs.push_back({ "SF Rush tracks", 1.0 });
            if (r1 != nullptr) jobs.push_back({ "SF Rush car stripes", 0.3 });
            // This computer's own times, from the last launch each job had work to do.
            for (Job& j : jobs) {
                auto it = learned.find(j.label);
                if (it != learned.end()) j.seconds = it->second;
            }
        }
        spawn(L"Prewarm Arcade", [] {
            job("Rush 2049 arcade mods", [] { rush2::arcade2049::refresh_now(); });
            std::thread cars([] {
                name_thread(L"Prewarm Car Mods", false);
                job("Car mods", [] { rush2::car_mods::prepare(); });
            });
            job("Custom tracks", [] { rush2::track_mod::prepare(); });
            cars.join();
        });
        if (r49 != nullptr) spawn(L"Prewarm 2049 Cars", [r49] { job("Rush 2049 cars", [&] { rush2::car2049::prepare(*r49); }); });
        if (cs != nullptr) spawn(L"Prewarm CS Cars", [cs] { job("California Speed cars", [&] { rush2::calspeed::prepare_cars(*cs); }); });
        spawn(L"Prewarm Car Stats", [] { job("Car stats", [] { rush2::car2049::prepare_bar_ranges(); }); });
        if (sf_rush) {
            spawn(L"Prewarm SF Rush", [r1] { job("SF Rush tracks", [&] { rush2::track1::prepare(*r1); }); });
        }
        if (r1 != nullptr) {
            spawn(L"Prewarm Stripes", [] { job("SF Rush car stripes", [] { rush2::car1stripes::prepare(); }); });
        }
        spawn(L"Prewarm Tracks", [] { job("Rush 2049 and California Speed tracks", [] { rush2::track2049::prepare(); }); });
        for (std::thread& t : threads) t.join();
        job("Track select art", [] { rush2::track2049::prepare_menu_container(); });
        {
            std::lock_guard lock{ progress_mutex };
            for (const Job& j : jobs) if (j.took >= 0.25) learned[j.label] = j.took;
        }
        save_times();
        return true;
    }

    void run(std::u8string game_id) {
        name_thread(L"Prewarm", false);
        auto started = std::chrono::steady_clock::now();
        bool built_all = build_pass(game_id);
        printf("[prewarm] launch builds %s (%.1f s, %llu cache misses%s)\n", built_all ? "done" : "skipped (the ROM didn't load)",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
               (unsigned long long)rush2::track_cache::misses(), rush2::arcade2049::rebuilt() ? ", arcade mods rebuilt" : "");
        fflush(stdout);
        pass_done = true;

        // From here on, in the background while the game runs: whatever a change in the menus needs.
        name_thread(L"Prewarm", true);
        auto r49 = rush2::wings::get_rom();
        auto cs = rush2::calspeed::get_rom();
        wait_for_rush2_rom();
        std::string built = tracks_signature();
        if (!built_all) {
            if (r49 != nullptr) rush2::car2049::prepare(*r49);
            if (cs != nullptr) rush2::calspeed::prepare_cars(*cs);
            rush2::car_mods::prepare();
            prepare_tracks();
        }
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

void rush2::prewarm::start(const std::u8string& game_id) {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(run, game_id).detach();
}

bool rush2::prewarm::ready() {
    // Test aid: RUSH2_TEST_BUILD_SCREEN=S keeps the launcher's progress screen up S seconds after the pass, to look at it.
    static const double hold = [] {
        const char* v = std::getenv("RUSH2_TEST_BUILD_SCREEN");
        return v != nullptr ? std::atof(v) : 0.0;
    }();
    if (!pass_done) return false;
    static const auto done_at = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - done_at).count() >= hold;
}

rush2::prewarm::Progress rush2::prewarm::progress() {
    std::lock_guard lock{ progress_mutex };
    Progress p;
    double total = 0.0, done = 0.0;
    auto now = std::chrono::steady_clock::now();
    for (const Job& j : jobs) {
        if (j.done) {
            total += j.took;
            done += j.took;
            continue;
        }
        total += j.seconds;
        if (j.started) {
            double ran = std::chrono::duration<double>(now - *j.started).count();
            done += j.seconds * (1.0 - std::exp(-ran / j.seconds));
            if (p.job.empty()) p.job = j.label;
        }
    }
    p.fraction = total > 0.0 ? float(done / total) : 0.0f;
    // Something is being built: a cache lookup missed or an arcade mod is being rebuilt. Not on a launch with
    // everything built, however long its checks take.
    p.building = rush2::track_cache::misses() > 0 || rush2::arcade2049::rebuilt();
    p.finished = !jobs.empty() && std::all_of(jobs.begin(), jobs.end(), [](const Job& j) { return j.done; });
    return p;
}

void rush2::prewarm::frame(uint8_t* rdram) {
    rush2::texture_prewarm::frame(rdram);
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
