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
}

int main(int argc, char** argv) {
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
