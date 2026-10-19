// Runs California Speed's billboard trees (src/calspeed/calspeed_billboards.cpp) outside the game on one track's
// files: writes the changed container and every card it looked at (cards.tsv, and each card's texels as crops.bin for
// the contact sheets), then checks the new lists: each one must end, call only the turn list and the container's own
// load lists, and draw each card as G_MTX push, G_DL turn, G_VTX 4, G_TRI2, G_POPMTX. Built and run by
// tools/calspeed/billboards_test.bat; tools/calspeed/billboards.py prepares the inputs and draws the sheets.
//
//     billboards_test.exe TRACK GEOMETRY PLACEMENT OUT_DIR [--any-texture]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "calspeed_billboards.h"

using Bytes = std::vector<uint8_t>;

static Bytes read_all(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static uint32_t u32(const Bytes& d, size_t o) {
    return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: billboards_test TRACK GEOMETRY PLACEMENT OUT_DIR [--any-texture]\n");
        return 2;
    }
    int t = atoi(argv[1]);
    Bytes g = read_all(argv[2]), p = read_all(argv[3]);
    std::string out = argv[4];
    bool any_texture = argc > 5 && std::string(argv[5]) == "--any-texture";
    size_t old_size = g.size();
    rush2::calspeed::BillboardResult r;
    bool ok = rush2::calspeed::add_billboards(t, g, p, r, true, any_texture);
    printf("%s", r.report.c_str());
    std::ofstream(out + "/geometry.bin", std::ios::binary).write(reinterpret_cast<const char*>(g.data()), std::streamsize(g.size()));

    FILE* tsv = fopen((out + "/cards.tsv").c_str(), "w");
    std::ofstream crops(out + "/crops.bin", std::ios::binary);
    fprintf(tsv, "chosen\twhy\tmodel\ttexture\tx\ty\tz\twidth\theight\tgreen\tclear\tcrop_w\tcrop_h\n");
    for (const auto& c : r.candidates) {
        fprintf(tsv, "%d\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.2f\t%.2f\t%d\t%d\n", c.chosen ? 1 : 0, c.why.c_str(), c.model.c_str(), c.texture.c_str(),
                c.base[0], c.base[1], c.base[2], c.width, c.height, c.green, c.clear, c.crop_w, c.crop_h);
        crops.write(reinterpret_cast<const char*>(c.crop.data()), std::streamsize(c.crop.size()));
    }
    fclose(tsv);
    if (!ok) return 0;

    // The new lists: every model record whose LOD 0 list moved past the old end.
    uint32_t models = u32(g, 4 * 4), table = u32(g, 0), loads0 = u32(g, 7 * 4), loads1 = u32(g, 8 * 4);
    uint32_t turn = 0;
    int lists = 0, drawn = 0, errors = 0;
    for (uint32_t i = 0; i < models; i++) {
        uint32_t list = u32(g, table + i * 0x34 + 12);
        if (list < old_size) continue;
        lists++;
        bool ended = false;
        int step = 0;   // within a card: 1 after G_MTX, 2 after the turn, 3 after G_VTX, 4 after G_TRI2
        for (uint32_t o = list; o + 8 <= g.size() && !ended; o += 8) {
            uint32_t w0 = u32(g, o), w1 = u32(g, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            auto bad = [&](const char* what) { if (errors++ < 10) printf("list %06X at %06X: %s\n", list, o, what); };
            if (op == 0xDF) { ended = true; if (step) bad("ends inside a card"); break; }
            if (op == 0xDA && w0 == 0xDA380000 && w1 >= old_size) { if (step) bad("matrix inside a card"); step = 1; continue; }
            if (op == 0xDE && step == 1) {
                if (!turn) turn = w1;
                if (w1 != turn || u32(g, turn) != 0xDA380001 || u32(g, turn + 4) != 0x0D000000 || u32(g, turn + 8) != 0xDF000000) bad("turn list");
                step = 2;
                continue;
            }
            if (op == 0x01 && step == 2) { if (w0 != 0x01004008 || w1 + 64 > g.size()) bad("card vertices"); step = 3; continue; }
            if (op == 0x06 && step == 3) { step = 4; continue; }
            if (op == 0xD8 && step == 4) { step = 0; drawn++; continue; }
            if (step) { bad("card commands out of order"); step = 0; }
            if (op == 0xDE) {
                uint32_t t = w1 & 0xFFFFFF;
                if (!(t >= loads0 && t < loads1) && t >= old_size) bad("calls a new list");
            }
        }
        if (!ended) { printf("list %06X runs off\n", list); errors++; }
    }
    printf("check: %d lists, %d cards drawn (of %d), %d errors\n", lists, drawn, r.cards, errors);
    return errors || drawn != r.cards ? 1 : 0;
}
