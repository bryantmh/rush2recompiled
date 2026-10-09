// Offline test of src/rush2049/track2049_art.cpp (track select dioramas and logos for the Rush 2049 tracks).
//
//     out\art_test.exe [repo root] [output dir]      (art_build.bat builds it and runs it from the repo root)
//
// Builds asset 3 the way the game does (build_menu_container on Rush 2's own asset 3 and the 2049 ROM), times it, then
// parses the result back: header and tables, sorted names, every model list run through a software RDP with address
// checks and the vertex chain walked as the loader does. Writes PNG previews to the output directory:
//     topdown_k.png     the top-down render of 2049 track k the diorama is textured with (route overlaid in red)
//     height_k.png      its height image
//     diorama_k.png     the finished R49TRACKk drawn from the container, from the track select's viewing angle
//     stock_vegas.png   Rush 2's VEGASTRACK drawn the same way, for comparison
//     asset3.bin        the container (python: model.R2Model(open(..).read()).check())
// ROMs: RUSH2049_ROM (default %LOCALAPPDATA%\Rush2Recompiled\rush2049.z64) and RUSH2_ROM (default
// <repo>/rush2.us.recomp.z64, Rush 2's main code uncompressed at 0x01000000 for the asset table).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "miniz.h"

#include "assets.h"

#include "../../../src/rush2049/track2049_art.cpp"

namespace fs = std::filesystem;

namespace {
    bool read_all(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    void write_all(const fs::path& path, const void* data, size_t size) {
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)data, (std::streamsize)size);
    }
    void write_png(const fs::path& path, const std::vector<std::array<uint8_t, 3>>& rgb, int w, int h) {
        size_t size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), w, h, 3, &size, 6, MZ_FALSE);
        write_all(path, png, size);
        mz_free(png);
    }

    // Perspective view of a model in a Rush 2 container, drawn by running its list.
    // cull: 0 none, -1 back faces as the game culls them (checked on VEGASTRACK), 1 front faces.
    // Translucent render modes (FORCE_BL without Z_UPD) blend half over what is behind.
    void render_model(const Bytes& c, uint32_t list, float yaw_deg, float pitch_deg, float distance, int w, int h,
                      std::vector<std::array<uint8_t, 3>>& img, int& errors, size_t& verts, size_t& tris, int cull = -1,
                      float tx = 0, float tz = 0, bool depth_test = true) {
        img.assign(size_t(w) * h, { 18, 22, 60 });
        std::vector<float> depth(size_t(w) * h, 1e30f);
        float yaw = yaw_deg * 3.14159265f / 180, pitch = pitch_deg * 3.14159265f / 180;
        float eye[3] = { std::sin(yaw) * std::cos(pitch) * distance, std::sin(pitch) * distance, std::cos(yaw) * std::cos(pitch) * distance };
        float fwd[3] = { -eye[0], -eye[1], -eye[2] };
        eye[0] += tx; eye[2] += tz;
        float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        for (float& f : fwd) f /= fl;
        float right[3] = { -fwd[2], 0, fwd[0] };
        float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        right[0] /= rl; right[2] /= rl;
        float up[3] = { right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0] };
        const float focal = h * 1.3f;

        Rdp rdp(c);
        rdp.transform = [](int16_t x, int16_t y, int16_t z, float out[3]) { out[0] = x; out[1] = y; out[2] = z; };
        rdp.triangle = [&](const RdpVertex& a, const RdpVertex& b, const RdpVertex& cc) {
            const RdpVertex* v[3] = { &a, &b, &cc };
            float p[3][2], iw[3];
            for (int i = 0; i < 3; i++) {
                float d[3] = { v[i]->x - eye[0], v[i]->y - eye[1], v[i]->z - eye[2] };
                float z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                if (z < 1) return;
                iw[i] = 1 / z;
                p[i][0] = w * 0.5f + (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) * focal * iw[i];
                p[i][1] = h * 0.5f - (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) * focal * iw[i];
            }
            float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
            if (cull && area * float(cull) > 0) return; // pixel y runs down: clockwise there is counter-clockwise up
            uint32_t rm = rdp.render_mode;
            bool translucent = (rm & 0x4000) && !(rm & 0x20);
            raster(p[0], p[1], p[2], w, h, [&](int x, int y, float l0, float l1, float l2) {
                float q0 = l0 * iw[0], q1 = l1 * iw[1], q2 = l2 * iw[2], sum = q0 + q1 + q2;
                float z = 1 / sum;
                size_t i = size_t(y) * w + x;
                if (depth_test && z >= depth[i]) return;
                q0 /= sum; q1 /= sum; q2 /= sum;
                float s = a.s * q0 + b.s * q1 + cc.s * q2, t = a.t * q0 + b.t * q1 + cc.t * q2, col[4], out[4];
                for (int k = 0; k < 4; k++) col[k] = a.c[k] * q0 + b.c[k] * q1 + cc.c[k] * q2;
                rdp.shade(s, t, col, out);
                if (translucent) {
                    for (int k = 0; k < 3; k++) img[i][k] = uint8_t(std::lround((img[i][k] / 255.0f * (1 - out[3]) + out[k] * out[3]) * 255));
                    return;
                }
                if ((rm & 0x1000) && out[3] < 0.5f) return; // alpha cut-out (CVG_X_ALPHA)
                depth[i] = z;
                for (int k = 0; k < 3; k++) img[i][k] = uint8_t(std::lround(out[k] * 255));
            });
        };
        rdp.run(list, 0x02 | 0x08);
        errors = rdp.errors;
        verts = rdp.vertices_loaded;
        tris = rdp.triangles;
    }

    // What the loader's relocation walk (func_8007786C) would touch in a model list: every pointer in range, then the
    // vertex chain from the first G_VTX (func_80077810). Returns the problems found.
    int check_list(const Bytes& c, uint32_t list, size_t& chain) {
        int problems = 0;
        uint32_t first_vtx = 0;
        for (uint32_t pc = list;; pc += 8) {
            if (pc + 8 > c.size()) { printf("    list runs off the file\n"); return problems + 1; }
            uint8_t op = c[pc];
            uint32_t w1 = be32(c, pc + 4);
            if (op == 0xDF) break;
            if (op == 0x01 || op == 0xDE || op == 0xDA || op == 0xDC || op == 0xFD || op == 0xFE || op == 0xFF) {
                if ((w1 & 0xFFFFFF) >= c.size()) { printf("    %06X: pointer %08X out of range\n", pc, w1); problems++; }
                if (op == 0xFD && (w1 & 7)) { printf("    %06X: texture image not 8-byte aligned\n", pc); problems++; }
                if (op == 0x01 && first_vtx == 0) first_vtx = w1 & 0xFFFFFF;
            }
        }
        chain = 0;
        for (uint32_t a = first_vtx;; a += 16) {
            if (a + 16 > c.size()) { printf("    vertex chain runs off the file\n"); return problems + 1; }
            chain++;
            if (!(be16(c, a + 6) & 0x8000)) break;
        }
        return problems;
    }
}

int main(int argc, char** argv) {
    fs::path repo = argc > 1 ? argv[1] : ".";
    fs::path out_dir = argc > 2 ? fs::path(argv[2]) : repo / "tools" / "rush2049" / "cpp_test" / "out" / "art";
    fs::create_directories(out_dir);
    const char* env49 = getenv("RUSH2049_ROM");
    const char* env2 = getenv("RUSH2_ROM");
    const char* appdata = getenv("LOCALAPPDATA");
    fs::path rom49_path = env49 ? fs::path(env49) : fs::path(appdata ? appdata : "") / "Rush2Recompiled" / "rush2049.z64";
    fs::path rom2_path = env2 ? fs::path(env2) : repo / "rush2.us.recomp.z64";
    Bytes rom49, rom2;
    if (!read_all(rom49_path, rom49) || !read_all(rom2_path, rom2)) {
        printf("Can't read %s or %s\n", rom49_path.string().c_str(), rom2_path.string().c_str());
        return 1;
    }
    auto source49 = rush2::rom2049::n64_source(std::make_shared<const std::vector<uint8_t>>(rom49));
    // Asset 3 (deflate) from Rush 2's asset table (0x800C185C) in the recomp ROM's uncompressed main code.
    uint32_t asset3_rom = be32(rom2, 0x01000000 + 0x800C185C - 0x800539E0 + 3 * 4);
    Bytes asset3;
    if (!rush2::assets::inflate_raw(rom2.data() + asset3_rom, rom2.size() - asset3_rom, asset3)) {
        printf("Can't read Rush 2's asset 3\n");
        return 1;
    }
    printf("Rush 2 asset 3: %zu bytes\n", asset3.size());

    // Each track's miniature on its own, timed, with its sizes.
    for (int k = 1; k <= 6; k++) {
        auto start = std::chrono::steady_clock::now();
        TrackModel tm;
        if (!build_track_model(*source49, k, tm)) {
            printf("track %d: build failed\n", k);
            return 1;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        size_t total = 0;
        for (const Bytes& part : tm.parts) total += part.size();
        printf("track %d: detail %d, %zu of %zu objects, %zu shapes, %zu down faces, %zu specks, %zu translucent dropped, %zu vertices, %zu triangles; texels %zu KB, vertices "
               "%zu KB, list %zu KB, loads %zu KB = %zu KB; radius %.1f; %.0f ms\n", k, tm.detail, tm.objects_drawn, tm.objects,
               tm.shapes_dropped, tm.faces_down, tm.specks, tm.translucent_dropped, tm.vertices, tm.triangles, tm.parts[texels_part].size() / 1024,
               tm.parts[vertices_part].size() / 1024, tm.parts[list_part].size() / 1024,
               tm.parts[loads_part].size() / 1024, total / 1024, tm.radius, ms);
        const TopDown& td = tm.print;
        auto img = td.rgb;
        auto plot = [&](const std::vector<Point>& pts) {
            for (const Point& p : pts) {
                int x = int(std::lround((p.x - td.x0) / td.pixel)), y = int(std::lround((p.z - td.z0) / td.pixel));
                if (x >= 0 && y >= 0 && x < td.w && y < td.h) img[size_t(y) * td.w + x] = { 255, 0, 0 };
            }
        };
        plot(td.spine);
        for (const auto& b : td.branches) plot(b);
        if (!img.empty()) write_png(out_dir / ("print_route_" + std::to_string(k) + ".png"), img, td.w, td.h);
    }

    // The container as the game builds it.
    auto start = std::chrono::steady_clock::now();
    Bytes c;
    if (!rush2::track2049::build_menu_container(asset3, *source49, c)) {
        printf("build_menu_container failed\n");
        return 1;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("build_menu_container: %.0f ms, %zu bytes (asset 3 was %zu)\n", ms, c.size(), asset3.size());
    write_all(out_dir / "asset3.bin", c.data(), c.size());

    // Parse it back.
    int problems = 0;
    uint32_t hd[10];
    for (int i = 0; i < 10; i++) hd[i] = be32(c, i * 4);
    if (hd[0] + hd[4] * 0x34 != hd[2] || hd[2] + hd[5] * 0x20 != hd[3] || hd[3] + hd[6] * 0x18 != hd[1] ||
        hd[1] + hd[4] * 0x18 != c.size() || hd[7] > hd[8] || hd[8] > c.size() || hd[9] != 0) {
        printf("header inconsistent\n");
        problems++;
    }
    std::vector<std::string> names;
    for (uint32_t i = 0; i < hd[4]; i++) names.push_back(name_of(c, hd[1] + i * 0x18));
    for (size_t i = 1; i < names.size(); i++) {
        if (names[i - 1].substr(0, 15) >= names[i].substr(0, 15)) { printf("names not sorted at %s\n", names[i].c_str()); problems++; }
    }
    for (uint32_t i = 1; i < hd[5]; i++) {
        if (name_of(c, hd[2] + (i - 1) * 0x20) >= name_of(c, hd[2] + i * 0x20)) { printf("textures not sorted\n"); problems++; }
    }
    for (uint32_t i = 0; i < hd[5]; i++) {
        uint32_t r = hd[2] + i * 0x20;
        int16_t pal = int16_t(be16(c, r + 0x16));
        if (be32(c, r + 0x18) >= c.size() || pal >= int(hd[6])) { printf("texture %u bad\n", i); problems++; }
    }
    for (uint32_t i = 0; i < hd[6]; i++) {
        if (be32(c, hd[3] + i * 0x18 + 0x14) + 512 > c.size()) { printf("palette %u bad\n", i); problems++; }
    }
    for (uint32_t i = 0; i < hd[4]; i++) {
        uint32_t r = hd[0] + i * 0x34;
        uint32_t lods = be32(c, r);
        if (lods < 1 || lods > 4) { printf("%s: %u LODs\n", names[i].c_str(), lods); problems++; continue; }
        for (uint32_t l = 0; l < lods; l++) {
            uint32_t list = be32(c, r + 4 + l * 12 + 8);
            if (list == 0) continue;
            size_t chain = 0;
            problems += check_list(c, list, chain);
            bool ours = names[i].rfind("R49TRACK", 0) == 0;
            bool vegas = names[i] == "VEGASTRACK";
            std::vector<std::array<uint8_t, 3>> img;
            int errors;
            size_t verts, tris;
            const int w = 960, h = 600;
            render_model(c, list, 25, 32, 1000.0f, w, h, img, errors, verts, tris);
            if (errors) { printf("%s: %d RDP errors\n", names[i].c_str(), errors); problems++; }
            if (ours || vegas) {
                printf("%-14s list %06X: %zu vertices loaded, chain %zu, %zu triangles, radius %.1f\n", names[i].c_str(),
                       list, verts, chain, tris, as_float(be32(c, hd[1] + i * 0x18 + 16)));
                std::string file = vegas ? "stock_vegas.png" : "diorama_" + names[i].substr(8) + ".png";
                write_png(out_dir / file, img, w, h);
                // Every model needs its vertex chain (loads may repeat vertices and conditionals skip some, so the
                // count isn't compared with the vertices loaded).
                if (ours && chain == 0) { printf("    no vertex chain\n"); problems++; }
                // The other side, and with the other culling (to check the winding convention).
                std::string stem = file.substr(0, file.size() - 4);
                render_model(c, list, 205, 32, 1000.0f, w, h, img, errors, verts, tris);
                write_png(out_dir / (stem + "_back.png"), img, w, h);
                render_model(c, list, 25, 32, 1000.0f, w, h, img, errors, verts, tris, 1);
                write_png(out_dir / (stem + "_otherside.png"), img, w, h);
                // ART_VIEW="n yaw pitch distance x z": an extra view of R49TRACKn aimed at model x, z.
                if (const char* view = getenv("ART_VIEW"); view && ours) {
                    int n;
                    float yaw, pitch, dist, x, z;
                    if (sscanf(view, "%d %f %f %f %f %f", &n, &yaw, &pitch, &dist, &x, &z) == 6 && names[i] == "R49TRACK" + std::to_string(n)) {
                        render_model(c, list, yaw, pitch, dist, w, h, img, errors, verts, tris, -1, x, z);
                        write_png(out_dir / (stem + "_view.png"), img, w, h);
                        render_model(c, list, yaw, pitch, dist, w, h, img, errors, verts, tris, -1, x, z, false);
                        write_png(out_dir / (stem + "_view_nodepth.png"), img, w, h);
                    }
                }
            }
        }
    }
    printf("%s: %d problems\n", problems ? "FAILED" : "OK", problems);
    return problems ? 1 : 0;
}
