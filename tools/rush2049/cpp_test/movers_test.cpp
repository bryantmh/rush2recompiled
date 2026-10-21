// Standalone test of src/rush2049/track2049_movers_logic.cpp: runs Rush 2049's path followers for race tracks 1-6 and prints
// what they do, to check the port by eye (trains loop, gondolas ping-pong, trap doors wait for their pads).
//
//     out\movers_test.exe [seconds] [track]      (movers_build.bat builds and runs it)
//
// Per track: every path object's spawn pose, then a line per object every 5 s of simulated time (position, node,
// direction, halted flag, speed), with a car driving onto every TRIGGER pad's group at 10 s and off at 11 s, and onto
// every one-shot (0x4000) object's group from 20 s to 22 s. It also checks the collision-group arithmetic: at spawn
// (rest pose) func_800BF838's rewrite of every MOVER polygon must give back the rest matrix and origin within one
// quantisation step, and it counts the group operations per kind.
// ROM: RUSH2049_ROM (default %LOCALAPPDATA%\Rush2Recompiled\rush2049.z64).
//
//     out\movers_test.exe --custom <mover_paths.bin> [seconds] [fps]
//
// A custom track's moving objects instead (the converter's mover_paths.bin, e.g. from tmp/track_mod_test's
// track_mod_test.exe <track folder> <out>), stepped at fps (default 60) from a few start phases: counts game frames
// where an object is drawn part-shrunk (scale between 0.01 and 0.99: a flip-book frame caught mid-step) and, per
// flip-book (frames sharing a pivot), frames where not exactly one of its frames shows.
//
//     out\movers_test.exe --script <mover_paths.bin> [laps] [lap seconds]
//
// A custom track's scripted moving objects (track.toml start / open / close, the SCRP and TRIG chunks): a race of
// `laps` laps (default 3) of `lap seconds` (default 40) is simulated at 60 fps, the cars released at 1 s, and every
// trigger box driven into once a lap (box b at 2 + 0.5 b s into each lap); with a LAPP chunk, a car drives the lap
// (progress events) and passes the last checkpoint 90 % into the last lap (last_checkpoint). Each scripted object's moves are printed
// (when it starts and stops, and how far from its spawn it is then), and every flip-book is checked to show exactly one
// frame throughout.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "assets.h"
#include "rush2049_rom.h"
#include "track2049_movers_logic.h"

namespace fs = std::filesystem;
using namespace rush2::track2049::movers;

namespace {
    bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
        return (uint16_t)((d[o] << 8) | d[o + 1]);
    }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
    }

    struct Mover {
        uint16_t group, poly;
        int16_t matrix[9];
        uint16_t vert;
        int16_t rest_vert[4];
    };

    // 2049 collision file: 0x10-byte header {u16 nSeg, nNode, nPoly, nVert, nMover, vlistBytes; u32 leafBytes},
    // then SEG (0x84), NODE (0x14), POLY (0x18), VERT (8), MOVER (0x20), ...
    std::vector<Mover> read_movers(const std::vector<uint8_t>& c) {
        std::vector<Mover> out;
        if (c.size() < 0x10) {
            return out;
        }
        size_t o = 0x10 + be16(c, 0) * 0x84 + be16(c, 2) * 0x14 + be16(c, 4) * 0x18 + be16(c, 6) * 8;
        int n = be16(c, 8);
        for (int i = 0; i < n && o + 0x20 <= c.size(); i++, o += 0x20) {
            Mover m;
            m.group = be16(c, o);
            m.poly = be16(c, o + 2);
            for (int k = 0; k < 9; k++) {
                m.matrix[k] = (int16_t)be16(c, o + 4 + k * 2);
            }
            m.vert = be16(c, o + 0x16);
            for (int k = 0; k < 4; k++) {
                m.rest_vert[k] = (int16_t)be16(c, o + 0x18 + k * 2);
            }
            out.push_back(m);
        }
        return out;
    }

    const char* model_name(Model m) {
        return m == Model::trigger_on ? " TRIGGERON" : m == Model::trigger_off ? " TRIGGEROFF" : "";
    }

    void print_object(const World& w, int i) {
        const Object& o = w.objects()[i];
        const Path& p = w.paths()[o.path];
        printf("    %2d %-11s p%-2d n%-2d %s%s pos %9.2f %9.2f %9.2f  v %7.2f%s", i, p.name.c_str(), o.path,
               o.f.node, (o.f.dir & 8) ? "bk" : "fw", (p.flags & path_halted) ? " H" : "  ", o.pos[0], o.pos[1],
               o.pos[2], o.f.speed, model_name(o.model));
        if (o.anim_count) {
            printf(" frame %d", o.anim_frame);
        }
        if (o.body) {
            printf(" body%d%s", o.body_index, o.cars ? "" : "(no car test)");
        }
        printf("\n");
    }

    // --custom: a custom track's mover_paths.bin, checked for flip-book steps (see the header).
    int run_custom(const char* file, float seconds, float fps) {
        std::vector<uint8_t> data;
        if (!read_file(file, data)) {
            printf("Can't read %s\n", file);
            return 1;
        }
        std::vector<Path> paths;
        if (!parse_paths(data.data(), data.size(), paths)) {
            printf("Bad paths file\n");
            return 1;
        }
        // As src/rush2049/track2049_movers.cpp sets up a custom track: each path its own type.
        std::vector<TypeInfo> types;
        for (const Path& p : paths) {
            TypeInfo t;
            t.name = p.name;
            t.model = p.name;
            t.flags = ((p.flags & path_wait_trigger) ? 5u << 16 : 0u) | 0x21;
            types.push_back(t);
        }
        Options options;
        options.custom = true;
        int failures = 0;
        for (int phase = 0; phase < 4; phase++) {
            World w;
            w.init(paths, types, options);
            w.update(phase * 0.0041f);
            // Flip-book frames: objects with a node of scale under 0.01 (hidden), in books by their pivot (the
            // first node's position; tools/arcade2049/track_mod.py gives a book's frames one pivot, and the game
            // draws them under one interpolation ID). Each book must show exactly one frame at every step.
            std::map<std::array<float, 3>, std::vector<int>> books;
            for (int i = 0; i < (int)w.objects().size(); i++) {
                const auto& nodes = w.paths()[w.objects()[i].path].nodes;
                for (const PathNode& n : nodes) {
                    if (n.scale[0] < 0.01f) {
                        books[{ nodes[0].pos[0], nodes[0].pos[1], nodes[0].pos[2] }].push_back(i);
                        break;
                    }
                }
            }
            int partial = 0, wrong = 0, frames = 0, steps = int(seconds * fps);
            for (const auto& [pivot, objs] : books) frames += (int)objs.size();
            for (int s = 0; s < steps; s++) {
                w.update(1.0f / fps);
                for (const auto& [pivot, objs] : books) {
                    int shown = 0;
                    for (int i : objs) {
                        const float* m = w.objects()[i].m;
                        float sc = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                        partial += sc > 0.01f && sc < 0.99f;
                        shown += sc >= 0.01f;
                    }
                    wrong += shown != 1;
                }
            }
            printf("phase %d: %zu objects, %d flip-book frames in %zu books, %d steps; part-shrunk frames %d, "
                   "book steps without exactly one frame %d\n",
                   phase, w.objects().size(), frames, books.size(), steps, partial, wrong);
            failures += partial + wrong;
        }
        printf(failures ? "FAIL\n" : "OK\n");
        return failures ? 1 : 0;
    }
}

namespace {
    // --script: a custom track's scripted movers through a simulated race (see the header).
    int run_script(const char* file, int laps, float lap_seconds) {
        std::vector<uint8_t> data;
        if (!read_file(file, data)) {
            printf("Can't read %s\n", file);
            return 1;
        }
        std::vector<Path> paths;
        std::vector<TriggerBox> boxes;
        if (!parse_paths(data.data(), data.size(), paths) || !parse_scripts(data.data(), data.size(), paths, boxes)) {
            printf("Bad paths or scripts\n");
            return 1;
        }
        std::vector<TypeInfo> types;
        for (const Path& p : paths) {
            TypeInfo t;
            t.name = p.name;
            t.model = p.name;
            t.flags = ((p.flags & path_wait_trigger) ? 5u << 16 : 0u) | 0x21;
            types.push_back(t);
        }
        Options options;
        options.custom = true;
        World w;
        w.init(paths, types, options);
        LapPath lap_path;
        if (parse_lap(data.data(), data.size(), lap_path)) w.set_lap(lap_path);
        static const char* modes[] = { "plain", "loop", "pingpong", "once", "door" };
        static const char* kinds[] = { "time", "trigger", "odd_lap", "even_lap", "final_lap", "progress", "last_checkpoint" };
        printf("%zu paths, %zu trigger boxes\n", paths.size(), boxes.size());
        for (size_t i = 0; i < w.paths().size(); i++) {
            const Script& sc = w.paths()[i].script;
            if (sc.mode == Script::plain) continue;
            printf("  %zu %s %s%s", i, w.paths()[i].name.c_str(), modes[sc.mode], sc.open_at_start ? " (open at start)" : "");
            const std::vector<ScriptEvent>* lists[3] = { &sc.start, &sc.open, &sc.close };
            const char* names[3] = { "start", "open", "close" };
            for (int l = 0; l < 3; l++) {
                for (const ScriptEvent& e : *lists[l]) {
                    printf(" %s:%s", names[l], kinds[e.kind]);
                    if (e.kind == ScriptEvent::time) printf(" %g", e.seconds);
                    if (e.kind == ScriptEvent::progress) printf(" %g ft", e.seconds);
                    if (e.kind == ScriptEvent::trigger) printf(" box %d lap %d", e.box, e.lap);
                }
            }
            if (sc.open_delay >= 0) printf(" open_delay %g", sc.open_delay);
            if (sc.close_delay >= 0) printf(" close_delay %g", sc.close_delay);
            printf("\n");
        }
        // Flip-books, by pivot (as --custom).
        std::map<std::array<float, 3>, std::vector<int>> books;
        for (int i = 0; i < (int)w.objects().size(); i++) {
            const auto& nodes = w.paths()[w.objects()[i].path].nodes;
            for (const PathNode& n : nodes) {
                if (n.scale[0] < 0.01f) {
                    books[{ nodes[0].pos[0], nodes[0].pos[1], nodes[0].pos[2] }].push_back(i);
                    break;
                }
            }
        }
        std::vector<std::array<float, 3>> spawn, last;
        for (const Object& o : w.objects()) spawn.push_back({ o.pos[0], o.pos[1], o.pos[2] }), last.push_back(spawn.back());
        std::vector<int> moving(w.objects().size(), 0), still_for(w.objects().size(), 0);
        std::vector<float> last_scale(w.objects().size(), 1.0f);
        const float dt = 1.0f / 60.0f;
        int steps = int((laps * lap_seconds + 5.0f) / dt), wrong = 0;
        std::vector<int> lap_of_box(boxes.size(), 0);
        for (int s = 0; s < steps; s++) {
            float t = s * dt;
            bool racing = t >= 1.0f;
            int lap = racing ? std::min(laps, 1 + int((t - 1.0f) / lap_seconds)) : 1;
            float into = racing ? t - 1.0f - (lap - 1) * lap_seconds : -1.0f;
            std::vector<std::vector<int>> entered(boxes.size());
            for (size_t b = 0; b < boxes.size(); b++) {
                if (racing && lap_of_box[b] < lap && into >= 2.0f + 0.5f * b) {
                    lap_of_box[b] = lap;
                    entered[b].push_back(lap);
                }
            }
            w.set_race(racing, lap, laps, entered);
            if (!lap_path.points.empty()) {
                // One car round the lap at a steady speed, a lap per lap_seconds; on the last lap it passes the last
                // checkpoint 90 % of the way round.
                float run = racing ? (t - 1.0f) / lap_seconds * lap_path.length : 0.0f;
                float d = std::fmod(run, lap_path.length);
                size_t i = size_t(std::upper_bound(lap_path.at.begin(), lap_path.at.end(), d) - lap_path.at.begin());
                i = i ? i - 1 : 0;
                const auto& a = lap_path.points[i];
                const auto& b = lap_path.points[(i + 1) % lap_path.points.size()];
                float seg = (i + 1 < lap_path.at.size() ? lap_path.at[i + 1] : lap_path.length) - lap_path.at[i];
                float f = seg > 0.0f ? (d - lap_path.at[i]) / seg : 0.0f;
                std::array<float, 3> pos;
                for (int k = 0; k < 3; k++) pos[k] = a[k] + (b[k] - a[k]) * f;
                w.set_cars({ pos }, { 1 }, racing && lap == laps && into >= 0.9f * lap_seconds);
            }
            w.update(dt);
            for (size_t i = 0; i < w.objects().size(); i++) {
                const Object& o = w.objects()[i];
                if (w.paths()[o.path].script.mode == Script::plain) continue;
                float d = 0.0f;
                for (int k = 0; k < 3; k++) d += (o.pos[k] - last[i][k]) * (o.pos[k] - last[i][k]);
                float sc = std::sqrt(o.m[0] * o.m[0] + o.m[1] * o.m[1] + o.m[2] * o.m[2]);
                // Moving, or still for under half a second (a slow mover's 1/32 ft steps come a few frames apart).
                bool changed = d > 1e-8f || std::fabs(sc - last_scale[i]) > 1e-4f;
                still_for[i] = changed ? 0 : still_for[i] + 1;
                bool now = changed || (moving[i] && still_for[i] < 30);
                if (now != (moving[i] != 0)) {
                    float off[3];
                    for (int k = 0; k < 3; k++) off[k] = o.pos[k] - spawn[i][k];
                    printf("  t=%6.2f lap %d  %zu %s %s at (%.1f, %.1f, %.1f) from its spawn, scale %.3f\n", t, lap, i,
                           w.paths()[o.path].name.c_str(), now ? "starts" : "stops", off[0], off[1], off[2], sc);
                    moving[i] = now;
                }
                last[i] = { o.pos[0], o.pos[1], o.pos[2] };
                last_scale[i] = sc;
            }
            for (const auto& [pivot, objs] : books) {
                int shown = 0;
                for (int i : objs) {
                    const float* m = w.objects()[i].m;
                    shown += std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) >= 0.01f;
                }
                wrong += shown != 1;
            }
        }
        printf("flip-books: %zu, steps without exactly one frame shown: %d\n%s\n", books.size(), wrong, wrong ? "FAIL" : "OK");
        return wrong ? 1 : 0;
    }
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--script") {
        return run_script(argv[2], argc > 3 ? atoi(argv[3]) : 3, argc > 4 ? (float)atof(argv[4]) : 40.0f);
    }
    if (argc > 2 && std::string(argv[1]) == "--custom") {
        return run_custom(argv[2], argc > 3 ? (float)atof(argv[3]) : 20.0f, argc > 4 ? (float)atof(argv[4]) : 60.0f);
    }
    float seconds = argc > 1 ? (float)atof(argv[1]) : 40.0f;
    int only = argc > 2 ? atoi(argv[2]) : 0;
    const char* env49 = getenv("RUSH2049_ROM");
    const char* appdata = getenv("LOCALAPPDATA");
    fs::path rom_path = env49 ? fs::path(env49) : fs::path(appdata ? appdata : "") / "Rush2Recompiled" / "rush2049.z64";
    std::vector<uint8_t> rom, main_data;
    if (!read_file(rom_path, rom)) {
        printf("Can't read %s\n", rom_path.string().c_str());
        return 1;
    }
    if (!rush2::assets::inflate_raw(rom.data() + 0xB0CB10, rom.size() - 0xB0CB10, main_data)) {
        printf("Can't inflate 2049 main data\n");
        return 1;
    }
    std::vector<TypeInfo> types;
    if (!parse_types(main_data.data(), main_data.size(), 0x80086A50, types)) {
        printf("Can't parse the type table\n");
        return 1;
    }

    // Math spot checks against values worked out from the libultra/2049 code paths.
    printf("sin(0.5)=%.9g cos(0.5)=%.9g acos(0.3)=%.9g acos(0.9)=%.9g\n", sinf2049(0.5f), cosf2049(0.5f),
           acosf2049(0.3f), acosf2049(0.9f));

    int failures = 0;
    const float dt = 1.0f / 30.0f;
    for (int k = 1; k <= 6; k++) {
        if (only && k != only) {
            continue;
        }
        for (int backward = 0; backward < 2; backward++) {
            std::vector<uint8_t> geometry, collision;
            if (!rush2::rom2049::read_file(rom, 100 + k, geometry) ||
                !rush2::rom2049::read_file(rom, 138 + k, collision)) {
                printf("track %d: can't read files\n", k);
                return 1;
            }
            std::vector<Path> paths;
            if (!parse_paths(geometry.data(), geometry.size(), paths)) {
                printf("track %d: bad PTHD\n", k);
                failures++;
                continue;
            }
            std::vector<Mover> movers = read_movers(collision);
            Options options;
            options.backward = backward != 0;
            World w;
            w.init(paths, types, options);
            printf("\n=== track %d %s: %zu paths, %zu objects, %zu collision movers ===\n", k,
                   backward ? "backward" : "forward", paths.size(), w.objects().size(), movers.size());

            // Rest-pose check: the spawn pose's func_800BF838 rewrite must reproduce the MOVER rest copies.
            std::map<int, int> checked;
            int worst_m = 0, worst_v = 0;
            for (const Object& o : w.objects()) {
                const Path& p = w.paths()[o.path];
                if (!(p.flags & path_moves_group) || checked.count(p.group)) {
                    continue;
                }
                checked[p.group] = 1;
                GroupOp op = {};
                op.kind = GroupOp::rotate;
                op.group = p.group;
                std::copy(o.pos, o.pos + 3, op.pivot);
                std::copy(o.f.inv_rest, o.f.inv_rest + 9, op.inv_rest);
                std::copy(o.m, o.m + 9, op.m);
                for (const Mover& m : movers) {
                    if (m.group != (uint16_t)p.group) {
                        continue;
                    }
                    int16_t om[9], ov[4];
                    apply_rotate(op, m.matrix, m.rest_vert, om, ov);
                    for (int i = 0; i < 9; i++) {
                        worst_m = std::max(worst_m, std::abs(om[i] - m.matrix[i]));
                    }
                    for (int i = 0; i < 3; i++) {
                        int a = m.rest_vert[i] * 32 + (((uint16_t)m.rest_vert[3] >> (10 - 5 * i)) & 0x1F);
                        int b = ov[i] * 32 + (((uint16_t)ov[3] >> (10 - 5 * i)) & 0x1F);
                        worst_v = std::max(worst_v, std::abs(a - b));
                    }
                }
            }
            printf("  rest-pose rewrite: %zu groups, worst matrix diff %d/16384, worst vertex diff %d/32\n",
                   checked.size(), worst_m, worst_v);
            if (worst_m > 2 || worst_v > 1) {
                printf("  REST POSE MISMATCH (only expected for objects whose spawn node is not their rest pose)\n");
            }

            std::map<int, int> op_counts;
            for (const GroupOp& op : w.take_group_ops()) {
                op_counts[op.kind]++;
            }
            printf("  init group ops: disable %d\n", op_counts[GroupOp::disable]);
            for (int i = 0; i < (int)w.objects().size(); i++) {
                print_object(w, i);
            }

            std::vector<int16_t> pads, one_shots;
            for (const Object& o : w.objects()) {
                const Path& p = w.paths()[o.path];
                if ((p.flags & (path_trigger_pad | path_linked_pad)) && o.slot == 5) {
                    pads.push_back(p.group);
                }
                if ((p.flags & path_one_shot) && (p.flags & path_wait_trigger) && o.slot == 5) {
                    one_shots.push_back(p.group);
                }
            }

            op_counts.clear();
            int frames = (int)(seconds / dt + 0.5f);
            for (int frame = 1; frame <= frames; frame++) {
                float now = frame * dt;
                if (frame == (int)(10.0f / dt)) {
                    for (int16_t g : pads) {
                        w.car_on_group(0, g, true);
                    }
                }
                if (frame == (int)(11.0f / dt)) {
                    for (int16_t g : pads) {
                        w.car_on_group(0, g, false);
                    }
                }
                if (frame >= (int)(20.0f / dt) && frame < (int)(22.0f / dt)) {
                    for (int16_t g : one_shots) {
                        w.car_on_group(1, g, true);
                    }
                }
                if (frame == (int)(22.0f / dt)) {
                    for (int16_t g : one_shots) {
                        w.car_on_group(1, g, false);
                    }
                }
                w.update(dt);
                for (const GroupOp& op : w.take_group_ops()) {
                    op_counts[op.kind]++;
                }
                for (const Object& o : w.objects()) {
                    for (float v : o.pos) {
                        if (!std::isfinite(v)) {
                            printf("  NON-FINITE position on path %d at %.2f s\n", o.path, now);
                            failures++;
                            break;
                        }
                    }
                }
                if (frame % (int)(5.0f / dt + 0.5f) == 0) {
                    printf("  t=%.1f s\n", now);
                    for (int i = 0; i < (int)w.objects().size(); i++) {
                        print_object(w, i);
                    }
                }
            }
            printf("  group ops over %.0f s: translate %d, rotate %d, restore %d, disable %d\n", seconds,
                   op_counts[GroupOp::translate], op_counts[GroupOp::rotate], op_counts[GroupOp::restore],
                   op_counts[GroupOp::disable]);
        }
    }
    printf("\n%s\n", failures ? "FAILURES" : "OK");
    return failures ? 1 : 0;
}
