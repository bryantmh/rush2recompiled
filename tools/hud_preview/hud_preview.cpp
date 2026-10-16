// Race HUD previewer (docs/hud_layout.md#previewer): lays out the race HUD with the game's own layout engine
// (src/hud_layout.cpp) from the HUD catalog (src/hud_catalog.cpp: every part the HUD can draw, from the ROM's layout
// tables and the game's callbacks) and draws it with the ROM's images. Nothing comes from the game, so any variant can
// be seen and checked without starting it.
//
//   hud_preview [options]                 one variant: a picture (OUT.png) and its issues
//     --set KEY=VALUE                      the variant (repeatable; defaults: players=1 aspect=16:9 odometer=1
//                                          manual=1111 mode=race):
//                                            players=1-4, layout=single|stacked|side|quads (2 players: stacked or side;
//                                            3-4 always quads), aspect=4:3|16:9|21:9|32:9 or hud_width=W,
//                                            odometer=0|1, manual=1010 (per player), mode=race|circuit|stunt|practice|
//                                            battle|race_battle (the BATTLE row), weapons=0|1 (the Weapons cheat's bar),
//                                            battle_time=0|1 (a battle's time limit), arena=0|1 (the stunt track),
//                                            stunts=0|1 (Stunts on All Tracks),
//                                            deaths=0|1 (the DEATHS option), it=0|1 (the stunt mode's option),
//                                            language=0-2, attract=0|1, and the HUD settings time, speed, position,
//                                            radar, map, time_left, tach, gear, lap_banner=0|1
//     --show all|none|ROLE,ROLE            which elements are drawn shown (default all that can appear); the rest are
//                                          outlined dashed
//     --out FILE.png  --zoom N             picture (default tmp/hud_preview/preview.png, zoom 3)
//     --json FILE                          every element's rule, place and bounds, every part as drawn (image, cell,
//                                          slide end) and the issues (what tools/hud_preview/editor.py draws)
//     --overrides FILE                     hand placements to use instead of the built-in ones (src/hud_overrides.inc's
//                                          format; the HUD editor's work in progress)
//     --no-picture                         no picture (with --json)
//     --parts                              lists every part: where the game draws it, its reach and its element
//   hud_preview --dump-images DIR          the HUD's ROM images as DIR/NAME.png (French and German: NAME.1.png,
//                                          NAME.2.png where they differ), for the HUD editor
//   hud_preview --matrix OUTDIR            every variant (players, layouts, aspects, odometer, gearbox, race, stunt
//                                          panel, stunt arena, DEATHS, HUD settings off): one picture each and
//                                          OUTDIR/index.txt with their issues; exit 1 on errors
//   hud_preview --truth CAPTURE            checks a capture of the game (tools/hud_preview/capture.py) against the
//                                          catalog and the engine: every widget has a part, is drawn inside its
//                                          part's reach and moved as the engine says
//   --rom PATH                             the Rush 2 recomp ROM (default rush2.us.recomp.z64; tools/extract.py)
//   --rom2049 PATH                         Rush 2049's ROM for the odometer's and battle bar's images (default
//                                          RUSH2049_ROM, else build/rush2049.z64)
//
// In pictures each element's outline is labelled with its role and player (dashed: it isn't shown in this picture;
// red: none of its rule's places was clear), a moving part's reach is dotted, the views are split by lines and the
// 4:3 screen's edges are marked.
//
// Build from Git Bash: cmd //c "tools\hud_preview\build.bat"   (writes tmp\hud_preview\hud_preview.exe)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "hud_layout.h"
#include "hud_rom.h"

using namespace rush2::hud_layout;
using rush2::hud_catalog::Kind;
namespace fs = std::filesystem;

// ---- A small JSON reader (the capture's) ----
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json& operator[](const char* key) const {
        static const Json none;
        for (const auto& kv : obj) {
            if (kv.first == key) return kv.second;
        }
        return none;
    }
    const Json& operator[](int i) const {
        static const Json none;
        return i >= 0 && (size_t)i < arr.size() ? arr[i] : none;
    }
    int i() const { return type == Bool ? (b ? 1 : 0) : (int)num; }
    double d() const { return num; }
    uint32_t hex() const { return (uint32_t)std::strtoul(str.c_str(), nullptr, 16); }
};

struct JsonParser {
    const char* p;
    void ws() {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    Json parse() {
        ws();
        Json j;
        if (*p == '{') {
            j.type = Json::Obj;
            p++;
            ws();
            if (*p == '}') { p++; return j; }
            while (true) {
                ws();
                Json k = parse();
                ws();
                p++; // :
                Json v = parse();
                j.obj.emplace_back(k.str, std::move(v));
                ws();
                if (*p == ',') { p++; continue; }
                p++; // }
                break;
            }
        }
        else if (*p == '[') {
            j.type = Json::Arr;
            p++;
            ws();
            if (*p == ']') { p++; return j; }
            while (true) {
                j.arr.push_back(parse());
                ws();
                if (*p == ',') { p++; continue; }
                p++; // ]
                break;
            }
        }
        else if (*p == '"') {
            j.type = Json::Str;
            p++;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == 'u') {
                        j.str += (char)std::strtol(std::string(p + 1, 4).c_str(), nullptr, 16);
                        p += 5;
                        continue;
                    }
                }
                j.str += *p++;
            }
            p++;
        }
        else if (std::strncmp(p, "null", 4) == 0) { p += 4; }
        else if (std::strncmp(p, "true", 4) == 0) { j.type = Json::Bool; j.b = true; p += 4; }
        else if (std::strncmp(p, "false", 5) == 0) { j.type = Json::Bool; p += 5; }
        else {
            j.type = Json::Num;
            char* end;
            j.num = std::strtod(p, &end);
            p = end;
        }
        return j;
    }
};

// ---- Variants ----
float aspect_width(const std::string& a) {
    if (a == "4:3") return 320.0f;
    if (a == "16:9") return 320.0f * (16.0f / 9.0f) / (4.0f / 3.0f);
    if (a == "21:9") return 320.0f * (21.0f / 9.0f) / (4.0f / 3.0f);
    if (a == "32:9") return 320.0f * (32.0f / 9.0f) / (4.0f / 3.0f);
    return std::strtof(a.c_str(), nullptr);
}

Config default_config() {
    Config c;
    c.players = 1;
    c.hud_width = aspect_width("16:9");
    c.widescreen = true;
    c.odometer = true;
    c.small_scale = 0.8f;
    for (bool& m : c.manual) m = true;
    return c;
}

// The arrangement a player count allows (2 players: stacked unless side is asked for; 3-4: quadrants).
void fix_arrangement(Config& c, const std::string& layout) {
    if (c.players >= 3) c.arrangement = Arrangement::Quadrants;
    else if (c.players == 2) c.arrangement = layout == "side" ? Arrangement::SideBySide : Arrangement::Single;
    else c.arrangement = Arrangement::Single;
}

bool apply_set(Config& c, std::string& layout, const std::string& key, const std::string& value) {
    bool on = value == "1";
    if (key == "players") c.players = std::clamp(std::atoi(value.c_str()), 1, 4);
    else if (key == "layout") layout = value;
    else if (key == "aspect") {
        c.hud_width = aspect_width(value);
        c.widescreen = value != "4:3";
    }
    else if (key == "hud_width") c.hud_width = std::strtof(value.c_str(), nullptr);
    else if (key == "odometer") c.odometer = on;
    else if (key == "manual") {
        for (int p = 0; p < 4; p++) c.manual[p] = p < (int)value.size() && value[p] == '1';
    }
    else if (key == "mode") {
        c.battle = value == "battle";
        c.race_type = value == "circuit" ? 1 : value == "stunt" || value == "battle" ? 2 : value == "practice" ? 3 : 0;
        c.battle_hud = value == "battle" ? 1 : value == "race_battle" ? 2 : 0;
        // The stunt mode is only played on the stunt track (track 11), as the battle arenas are hosted there.
        c.stunt_arena = value == "battle" || value == "stunt";
    }
    else if (key == "weapons") c.battle_hud = on ? 3 : c.battle_hud == 3 ? 0 : c.battle_hud;
    else if (key == "race_battle") c.battle_hud = on ? 2 : c.battle_hud == 2 ? 0 : c.battle_hud;
    else if (key == "battle_time") c.battle_time_limit = on;
    else if (key == "arena") c.stunt_arena = on;
    else if (key == "stunts") c.stunts_all_tracks = on;
    else if (key == "deaths") c.deaths = on;
    else if (key == "it") c.mode_option = on;
    else if (key == "language") c.language = std::clamp(std::atoi(value.c_str()), 0, 2);
    else if (key == "attract") c.attract = on;
    else if (key == "time") c.shown.time = on;
    else if (key == "speed") c.shown.speed = on;
    else if (key == "position") c.shown.position = on;
    else if (key == "radar") c.shown.radar = on;
    else if (key == "map") c.shown.map = on;
    else if (key == "time_left") c.shown.time_left = on;
    else if (key == "tach") c.shown.tach = on;
    else if (key == "gear") c.shown.gear = on;
    else if (key == "lap_banner") c.shown.lap_banner = on;
    else return false;
    return true;
}

// ---- Drawing ----
struct Canvas {
    int w, h, zoom;
    float hud_width;
    std::vector<uint8_t> px;
    Canvas(float hud_w, int z) : zoom(z), hud_width(hud_w) {
        w = (int)std::lround(hud_w * z);
        h = screen_height * z;
        px.assign((size_t)w * h * 4, 0);
        for (int i = 0; i < w * h; i++) {
            px[i * 4 + 0] = 58;
            px[i * 4 + 1] = 64;
            px[i * 4 + 2] = 74;
            px[i * 4 + 3] = 255;
        }
    }
    // Screen x (widened 4:3 coordinates, 0 at the 4:3 screen's left edge) to canvas pixels.
    int sx(float x) const { return (int)std::floor((x + (hud_width - screen_width) / 2.0f) * zoom); }
    int sy(float y) const { return (int)std::floor(y * zoom); }
    void blend(int x, int y, uint32_t rgba) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* o = &px[((size_t)y * w + x) * 4];
        float a = (rgba & 0xFF) / 255.0f;
        o[0] = (uint8_t)(o[0] * (1 - a) + ((rgba >> 24) & 0xFF) * a);
        o[1] = (uint8_t)(o[1] * (1 - a) + ((rgba >> 16) & 0xFF) * a);
        o[2] = (uint8_t)(o[2] * (1 - a) + ((rgba >> 8) & 0xFF) * a);
    }
    void fill(float x0, float y0, float x1, float y1, uint32_t rgba) {
        for (int y = sy(y0); y < sy(y1); y++) {
            for (int x = sx(x0); x < sx(x1); x++) blend(x, y, rgba);
        }
    }
    void outline(const Rect& r, uint32_t rgba, int dash) {
        int x0 = sx((float)r.x0), y0 = sy((float)r.y0), x1 = sx((float)r.x1 + 1) - 1, y1 = sy((float)r.y1 + 1) - 1;
        for (int x = x0; x <= x1; x++) {
            if (dash && ((x - x0) / dash) % 2) continue;
            blend(x, y0, rgba);
            blend(x, y1, rgba);
        }
        for (int y = y0; y <= y1; y++) {
            if (dash && ((y - y0) / dash) % 2) continue;
            blend(x0, y, rgba);
            blend(x1, y, rgba);
        }
    }
    // Texels (sx0, sy0) - (sx1, sy1) of an image over the screen rectangle (x0, y0) - (x1, y1) (exclusive), point
    // sampled, optionally tinted (an intensity image the game tints with the primitive color).
    void image(const hud_rom::Image& im, int tx0, int ty0, int tw, int th, float x0, float y0, float x1, float y1,
               float alpha, uint32_t tint) {
        int px0 = sx(x0), py0 = sy(y0), px1 = sx(x1), py1 = sy(y1);
        for (int y = py0; y < py1; y++) {
            int ty = ty0 + std::clamp((int)((y - py0 + 0.5f) * th / (float)std::max(1, py1 - py0)), 0, th - 1);
            if (ty < 0 || ty >= im.h) continue;
            for (int x = px0; x < px1; x++) {
                int tx = tx0 + std::clamp((int)((x - px0 + 0.5f) * tw / (float)std::max(1, px1 - px0)), 0, tw - 1);
                if (tx < 0 || tx >= im.w) continue;
                const uint8_t* t = &im.rgba[((size_t)ty * im.w + tx) * 4];
                if (t[3] == 0) continue;
                uint32_t r = t[0], g = t[1], b = t[2];
                if (tint) {
                    r = r * ((tint >> 24) & 0xFF) / 255;
                    g = g * ((tint >> 16) & 0xFF) / 255;
                    b = b * ((tint >> 8) & 0xFF) / 255;
                }
                blend(x, y, (r << 24) | (g << 16) | (b << 8) | (uint32_t)(t[3] * alpha));
            }
        }
    }
    // 3 x 5 pixel letters, drawn at `scale` canvas pixels each.
    void text(int x, int y, const std::string& s, uint32_t rgba, int scale = 1) {
        static const std::map<char, const char*> font = {
            { 'A', "010101111101101" }, { 'B', "110101110101110" }, { 'C', "011100100100011" }, { 'D', "110101101101110" },
            { 'E', "111100110100111" }, { 'F', "111100110100100" }, { 'G', "011100101101011" }, { 'H', "101101111101101" },
            { 'I', "111010010010111" }, { 'J', "001001001101010" }, { 'K', "101101110101101" }, { 'L', "100100100100111" },
            { 'M', "101111111101101" }, { 'N', "110101101101101" }, { 'O', "010101101101010" }, { 'P', "110101110100100" },
            { 'Q', "010101101110011" }, { 'R', "110101110101101" }, { 'S', "011100010001110" }, { 'T', "111010010010010" },
            { 'U', "101101101101111" }, { 'V', "101101101101010" }, { 'W', "101101111111101" }, { 'X', "101101010101101" },
            { 'Y', "101101010010010" }, { 'Z', "111001010100111" }, { '0', "111101101101111" }, { '1', "010110010010111" },
            { '2', "110001010100111" }, { '3', "110001010001110" }, { '4', "101101111001001" }, { '5', "111100110001110" },
            { '6', "011100111101111" }, { '7', "111001010010010" }, { '8', "111101111101111" }, { '9', "111101111001110" },
            { ':', "000010000010000" }, { '.', "000000000000010" }, { '/', "001001010100100" }, { '-', "000000111000000" },
        };
        int cx = x;
        for (char ch : s) {
            auto it = font.find((char)std::toupper((unsigned char)ch));
            if (it != font.end()) {
                for (int r = 0; r < 5; r++) {
                    for (int col = 0; col < 3; col++) {
                        if (it->second[r * 3 + col] != '1') continue;
                        for (int yy = 0; yy < scale; yy++) {
                            for (int xx = 0; xx < scale; xx++) blend(cx + col * scale + xx, y + r * scale + yy, rgba);
                        }
                    }
                }
            }
            cx += 4 * scale;
        }
    }
    bool save(const std::string& path) {
        return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
    }
};

uint32_t role_color(Role r) {
    static const uint32_t colors[] = { 0xB0B0B0FF, 0x4FC3F7FF, 0xFFB74DFF, 0x81C784FF, 0xBA68C8FF, 0xFFF176FF, 0xE57373FF,
                                       0x4DD0E1FF, 0xA1887FFF, 0xF06292FF, 0x9575CDFF, 0xAED581FF, 0xDCE775FF, 0x64B5F6FF,
                                       0xFFFFFFFF, 0xFF8A65FF, 0x90A4AEFF, 0xFFD54FFF };
    return colors[(int)r % (sizeof(colors) / sizeof(colors[0]))];
}

std::string element_name(const Element& e) {
    std::string n = role_name(e.role);
    if (e.player >= 0) n = "P" + std::to_string(e.player + 1) + " " + n;
    return n;
}

// A rectangle of a part as drawn: moved, scaled about the pivot, and widened by its origin.
Rect drawn_rect(const Output& out, const Place& pl, const Rect& r) {
    if (!r.valid()) return r;
    Rect m = r;
    if (pl.placed) {
        m = r.moved(pl.dx, pl.dy);
        if (pl.scale != 0.0f) {
            auto s = [&](int32_t v, int32_t pivot) { return pivot + (int32_t)std::lround((v - pivot) * pl.scale); };
            m = Rect{ s(m.x0, pl.pivot_x), s(m.y0, pl.pivot_y), s(m.x1 + 1, pl.pivot_x) - 1, s(m.y1 + 1, pl.pivot_y) - 1 };
        }
    }
    int32_t o = origin_offset(pl.placed ? pl.origin : origin_none, out.config.hud_width);
    return m.moved(o, 0);
}

// An image the recomp draws for an element with no game widgets (Rush 2049's odometer digits, the battle bar's
// frame), from Rush 2049's ROM: a source rectangle of images2049[tex] drawn over r.
struct Sprite {
    int element = -1;
    std::string tex;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    Rect r;
};
std::vector<Sprite> code_sprites(const Output& out, const hud_rom::Rom& rom) {
    std::vector<Sprite> v;
    for (size_t i = 0; i < out.elements.size(); i++) {
        const Element& e = out.elements[i];
        if (!(e.possible || e.hidden) || !e.box.valid()) continue;
        if (e.role == Role::Odometer) {
            // rush2::odometer2049::draw: four digit cells, the whole digits from the image's left column and the
            // tenths from its right one (0-9 and 0 from the top). Shown: 027.4.
            auto it = rom.images2049.find("ODOMETER");
            if (it == rom.images2049.end()) continue;
            const hud_rom::Image& im = it->second;
            int cw = im.w / 2, ch = im.h / 11;
            int cell = e.box.w() / 4;
            static const int digits[4] = { 0, 2, 7, 4 };
            for (int d = 0; d < 4; d++) {
                Sprite s;
                s.element = (int)i;
                s.tex = "ODOMETER";
                s.sx = d < 3 ? 0 : cw;
                s.sy = digits[d] * ch;
                s.sw = cw;
                s.sh = ch;
                s.r = Rect{ e.box.x0 + d * cell, e.box.y0, e.box.x0 + (d + 1) * cell - 1, e.box.y1 };
                v.push_back(s);
            }
        }
        else if (e.role == Role::BattleBar) {
            auto it = rom.images2049.find("HEALTHBG");
            if (it == rom.images2049.end()) continue;
            Sprite s;
            s.element = (int)i;
            s.tex = "HEALTHBG";
            s.sw = it->second.w;
            s.sh = it->second.h;
            s.r = e.box;
            v.push_back(s);
        }
    }
    return v;
}

bool shows(const std::set<Role>& roles, const std::string& show, const Element& e) {
    if (!e.possible) return false;
    if (show.empty() || show == "all") return true;
    if (show == "none") return false;
    return roles.count(e.role) != 0;
}

void draw(const Output& out, const hud_rom::Rom& rom, Canvas& cv, const std::string& show, bool labels,
          const std::string& title) {
    std::set<Role> roles;
    if (!show.empty() && show != "all" && show != "none") {
        std::stringstream ss(show);
        std::string r;
        while (std::getline(ss, r, ',')) {
            for (int k = 0; k < (int)Role::Count; k++) {
                if (r == role_name((Role)k)) roles.insert((Role)k);
            }
        }
    }
    const Config& c = out.config;
    // The views.
    uint32_t line = 0x000000FF;
    if (c.arrangement == Arrangement::SideBySide || c.arrangement == Arrangement::Quadrants) {
        cv.fill(screen_width / 2.0f - 0.5f, 0, screen_width / 2.0f + 0.5f, (float)screen_height, line);
    }
    if (c.arrangement == Arrangement::Quadrants || (c.arrangement == Arrangement::Single && c.players == 2)) {
        cv.fill(-1000, screen_height / 2.0f - 0.5f, 1000, screen_height / 2.0f + 0.5f, line);
    }
    if (c.hud_width > screen_width) {
        for (float x : { 0.0f, (float)screen_width }) {
            for (int y = 0; y < cv.h; y += 6) cv.fill(x - 0.2f, y / (float)cv.zoom, x + 0.2f, (y + 3) / (float)cv.zoom, 0xFFFFFF40);
        }
    }
    // The parts, in the game's order.
    for (size_t i = 0; i < out.parts.size(); i++) {
        const Part& p = out.parts[i];
        const Place& pl = out.places[i];
        if (!p.present || pl.element < 0) continue;
        const Element& e = out.elements[pl.element];
        if (!shows(roles, show, e)) continue;
        auto draw_part = [&](const Rect& r, float alpha) {
            if (!r.valid()) return;
            if (p.kind == Kind::Fill) {
                uint32_t a = (uint32_t)std::lround((p.color & 0xFF) * alpha);
                cv.fill((float)r.x0, (float)r.y0, (float)r.x1 + 1, (float)r.y1 + 1, (p.color & 0xFFFFFF00) | a);
            }
            else if (p.kind == Kind::Text) {
                cv.text(cv.sx((float)r.x0), cv.sy((float)r.y0), p.text, 0xFFFFFFFF, std::max(1, cv.zoom * 3 / 4));
            }
            else if (const hud_rom::Image* im = rom.image(p.texture, c.language)) {
                int tw = p.cell_w ? p.cell_w : im->w, th = p.cell_h ? p.cell_h : im->h;
                int cols = std::max(1, im->w / std::max(1, tw));
                int tx = p.cell_w ? (p.cell % cols) * tw : 0, ty = p.cell_h ? (p.cell / cols) * th : 0;
                bool intensity = std::strncmp(p.texture, "DIGIT", 5) == 0;
                cv.image(*im, tx, ty, tw, th, (float)r.x0, (float)r.y0, (float)r.x1 + 1, (float)r.y1 + 1, alpha,
                         intensity ? 0xFFD820FF : 0);
            }
        };
        Rect r = drawn_rect(out, pl, p.at);
        if (!r.valid()) continue;
        draw_part(r, 1.0f);
        if (p.end.valid()) {
            // A sliding part (WRONG WAY): faded where the slide ends, with its anchor there, and its path dotted.
            Place at_end = pl;
            at_end.origin = pl.origin_end;
            Rect r1 = drawn_rect(out, at_end, p.end);
            draw_part(r1, 0.45f);
            if (labels) {
                float x0 = (r.x0 + r.x1) / 2.0f, y0 = (r.y0 + r.y1) / 2.0f;
                float x1 = (r1.x0 + r1.x1) / 2.0f, y1 = (r1.y0 + r1.y1) / 2.0f;
                for (int k = 0; k <= 24; k += 2) {
                    float f = k / 24.0f, x = x0 + (x1 - x0) * f, y = y0 + (y1 - y0) * f;
                    cv.fill(x - 0.6f, y - 0.6f, x + 0.6f, y + 0.6f, 0xFFFFFFC0);
                }
            }
        }
        else if (labels && !(p.reach == p.at)) cv.outline(drawn_rect(out, pl, p.reach), 0xFFFFFF50, 1);
    }
    // The odometer (src/rush2049/odometer.cpp): five digit columns on a dark panel.
    for (int pi = 0; pi < 4; pi++) {
        const OdometerPlace& o = out.odometer[pi];
        int ei = out.element(Role::Odometer, pi);
        if (!o.valid || ei < 0 || !shows(roles, show, out.elements[ei])) continue;
        const Rect& b = out.elements[ei].box;
        if (rom.images2049.count("ODOMETER")) continue;  // drawn with Rush 2049's digits (below)
        cv.fill((float)b.x0, (float)b.y0, (float)b.x1 + 1, (float)b.y1 + 1, 0x101010E0);
        int cell = (b.w()) / 4;
        for (int d = 0; d < 4; d++) {
            cv.fill((float)b.x0 + d * cell + 1, (float)b.y0 + 1, (float)b.x0 + (d + 1) * cell - 1, (float)b.y1, d < 3 ? 0xE0E0E0FF : 0xF0F0F0FF);
        }
    }
    for (const Sprite& s : code_sprites(out, rom)) {
        if (!shows(roles, show, out.elements[s.element])) continue;
        const hud_rom::Image& im = rom.images2049.at(s.tex);
        cv.image(im, s.sx, s.sy, s.sw, s.sh, (float)s.r.x0, (float)s.r.y0, (float)s.r.x1 + 1, (float)s.r.y1 + 1, 1.0f, 0);
        if (s.tex == "HEALTHBG") {
            // The green fill in its 56 x 4 window, over the frame (battle.cpp update_hud: its widget is built after).
            float sx = s.r.w() / 64.0f, sy = s.r.h() / 8.0f;
            cv.fill(s.r.x0 + 4 * sx, s.r.y0 + 2 * sy, s.r.x0 + 4 * sx + 42 * sx, s.r.y0 + 6 * sy, 0x008000FF);
        }
    }
    // Rush 2049's battle HUD (no Rush 2 images): the health bar's frame and fill, the models as plain shapes with the
    // digits printed on them.
    for (const Element& e : out.elements) {
        if (!e.possible || e.spot < 0 || !shows(roles, show, e)) continue;
        const Rect& b = e.box;
        auto fill = [&](const Rect& r, uint32_t rgba) { cv.fill((float)r.x0, (float)r.y0, (float)r.x1 + 1, (float)r.y1 + 1, rgba); };
        int z = std::max(1, cv.zoom * 3 / 4);
        switch (e.role) {
            case Role::BattleBar: {
                if (rom.images2049.count("HEALTHBG")) break;  // drawn with Rush 2049's frame (above)
                fill(b, 0x303030FF);
                float sx = b.w() / 64.0f, sy = b.h() / 8.0f;
                cv.fill(b.x0 + 4 * sx, b.y0 + 2 * sy, b.x0 + 4 * sx + 42 * sx, b.y0 + 6 * sy, 0x00A000FF);
                break;
            }
            case Role::BattleWeapon:
            case Role::BattleCoin:
            case Role::BattlePowerup: {
                uint32_t color = e.role == Role::BattleWeapon ? 0xC07030C0 : e.role == Role::BattleCoin ? 0xD0B020C0 : 0x4070D0C0;
                fill(b, color);
                if (e.role != Role::BattlePowerup) cv.text(cv.sx((float)b.x0) + 2, cv.sy((float)(b.y0 + b.y1) / 2) - 2 * z, "999", 0xFFFFFFFF, z);
                break;
            }
            default:
                break;
        }
    }
    if (labels) {
        for (const Element& e : out.elements) {
            if (!e.possible || e.spot < 0) continue;
            bool shown = shows(roles, show, e);
            uint32_t col = e.cramped ? 0xFF3030FF : role_color(e.role);
            cv.outline(e.box, col, shown ? 0 : 3);
            cv.text(cv.sx((float)e.box.x0) + 2, cv.sy((float)e.box.y0) + 2, element_name(e), col, 1);
        }
    }
    cv.text(4, cv.h - 10, title, 0xFFFFFFFF, 1);
}

// ---- Issues ----
std::string issues_text(const std::vector<Issue>& issues, int& errors, int& notes) {
    std::string s;
    errors = notes = 0;
    for (const Issue& i : issues) {
        s += std::string(i.error ? "    ERROR " : "    note  ") + i.text + "\n";
        (i.error ? errors : notes)++;
    }
    return s;
}

std::string json_string(const std::string& v) {
    std::string r = "\"";
    for (char ch : v) {
        if (ch == '"' || ch == '\\') r += '\\';
        if ((unsigned char)ch >= 0x20) r += ch;
    }
    return r + "\"";
}

std::string rect_json(const Rect& r) {
    if (!r.valid()) return "null";
    char b[64];
    std::snprintf(b, sizeof(b), "[%d,%d,%d,%d]", r.x0, r.y0, r.x1, r.y1);
    return b;
}

std::string elements_json(const Output& out, const std::vector<Issue>& issues, const hud_rom::Rom* rom = nullptr) {
    const Config& c = out.config;
    char b[1024];
    std::snprintf(b, sizeof(b),
                  "{\"config\":{\"layout\":\"%s\",\"players\":%d,\"arrangement\":%d,\"hud_width\":%.3f,\"language\":%d},"
                  "\"elements\":[",
                  layout_name(layout_id(c)), c.players, (int)c.arrangement, c.hud_width, c.language);
    std::string s = b;
    bool first = true;
    for (size_t i = 0; i < out.elements.size(); i++) {
        const Element& e = out.elements[i];
        std::snprintf(b, sizeof(b),
                      "%s{\"index\":%zu,\"name\":%s,\"role\":\"%s\",\"player\":%d,\"group\":%d,\"possible\":%d,\"hidden\":%d,"
                      "\"by_hand\":%d,\"rule\":%s,\"spot\":%d,\"cramped\":%d,\"origin\":%u,\"origin_end\":%u,\"slides\":%d,"
                      "\"scale\":%.3f,\"x\":%d,\"y\":%d,\"game\":%s,\"box\":%s}",
                      first ? "" : ",", i, json_string(element_name(e)).c_str(), role_name(e.role), e.player, e.group,
                      e.possible, e.hidden, e.by_hand, json_string(e.rule).c_str(), e.spot, e.cramped, e.origin, e.origin_end,
                      e.slides, e.scale, e.x, e.y, rect_json(e.game).c_str(), rect_json(e.box).c_str());
        s += b;
        first = false;
    }
    // The parts as drawn (an element hidden by hand: where the game would draw it), for the editor.
    s += "],\"parts\":[";
    first = true;
    for (size_t i = 0; i < out.parts.size(); i++) {
        const Part& p = out.parts[i];
        const Place& pl = out.places[i];
        if (!p.present || pl.element < 0) continue;
        Rect r = drawn_rect(out, pl, p.at);
        if (!r.valid()) continue;
        Rect r1;
        if (p.end.valid()) {
            Place at_end = pl;
            at_end.origin = pl.origin_end;
            r1 = drawn_rect(out, at_end, p.end);
        }
        int tx = 0, ty = 0, tw = 0, th = 0;
        if (p.kind == Kind::Image && rom != nullptr) {
            if (const hud_rom::Image* im = rom->image(p.texture, c.language)) {
                tw = p.cell_w ? p.cell_w : im->w;
                th = p.cell_h ? p.cell_h : im->h;
                int cols = std::max(1, im->w / std::max(1, tw));
                tx = p.cell_w ? (p.cell % cols) * tw : 0;
                ty = p.cell_h ? (p.cell / cols) * th : 0;
            }
        }
        std::snprintf(b, sizeof(b),
                      "%s{\"e\":%d,\"kind\":%d,\"tex\":%s,\"cell\":[%d,%d,%d,%d],\"color\":%u,\"text\":%s,\"tint\":%d,"
                      "\"r\":%s,\"end\":%s}",
                      first ? "" : ",", pl.element, (int)p.kind, json_string(p.texture).c_str(), tx, ty, tw, th, p.color,
                      json_string(p.text).c_str(), std::strncmp(p.texture, "DIGIT", 5) == 0, rect_json(r).c_str(),
                      rect_json(r1).c_str());
        s += b;
        first = false;
    }
    // Per player: the trick name's box is in the scores' place, so the game shows one of them at a time.
    std::snprintf(b, sizeof(b), "],\"stunt_swap\":[%d,%d,%d,%d]", out.stunt_swap[0], out.stunt_swap[1], out.stunt_swap[2],
                  out.stunt_swap[3]);
    s += b;
    s += ",\"sprites\":[";
    first = true;
    if (rom != nullptr) {
        for (const Sprite& sp : code_sprites(out, *rom)) {
            std::snprintf(b, sizeof(b), "%s{\"e\":%d,\"tex\":%s,\"cell\":[%d,%d,%d,%d],\"r\":%s}", first ? "" : ",",
                          sp.element, json_string("2049_" + sp.tex).c_str(), sp.sx, sp.sy, sp.sw, sp.sh, rect_json(sp.r).c_str());
            s += b;
            first = false;
        }
    }
    s += "],\"issues\":[";
    first = true;
    for (const Issue& i : issues) {
        s += std::string(first ? "" : ",") + "{\"error\":" + (i.error ? "true" : "false") + ",\"text\":" + json_string(i.text) + "}";
        first = false;
    }
    return s + "]}\n";
}

// Hand placements in src/hud_overrides.inc's format: one "{ LayoutId::P4, Role::Deaths, 3, 0, 1, 120, 200, 0x400,
// 1.0f }," per line ("//" comments).
bool read_overrides(const std::string& path, std::vector<Override>& list, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "can't read " + path;
        return false;
    }
    static const char* const ids[] = { "LayoutId::P1", "LayoutId::P2Stacked", "LayoutId::P2Side", "LayoutId::P3", "LayoutId::P4" };
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        n++;
        size_t open = line.find('{');
        size_t comment = line.find("//");
        if (open == std::string::npos || comment < open) continue;
        std::string body = line.substr(open + 1, line.find('}') - open - 1);
        std::vector<std::string> v;
        std::stringstream ss(body);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \tf") + 1);
            v.push_back(item);
        }
        if (v.size() != 9 && v.size() != 10 && v.size() != 13) {
            error = path + ":" + std::to_string(n) + ": expected 9, 10 or 13 fields";
            return false;
        }
        Override o;
        bool layout_ok = false, role_ok = false;
        for (int k = 0; k < (int)LayoutId::Count; k++) {
            if (v[0] == ids[k]) {
                o.layout = (LayoutId)k;
                layout_ok = true;
            }
        }
        for (int k = 0; k < (int)Role::Count; k++) {
            if (v[1] == std::string("Role::") + role_name((Role)k)) {
                o.role = (Role)k;
                role_ok = true;
            }
        }
        if (!layout_ok || !role_ok) {
            error = path + ":" + std::to_string(n) + ": unknown layout or role";
            return false;
        }
        o.player = (int8_t)std::strtol(v[2].c_str(), nullptr, 0);
        o.group = (uint8_t)std::strtol(v[3].c_str(), nullptr, 0);
        o.flags = (uint8_t)std::strtol(v[4].c_str(), nullptr, 0);
        o.x = (int32_t)std::strtol(v[5].c_str(), nullptr, 0);
        o.y = (int32_t)std::strtol(v[6].c_str(), nullptr, 0);
        o.origin = (uint16_t)std::strtol(v[7].c_str(), nullptr, 0);
        o.scale = std::strtof(v[8].c_str(), nullptr);
        if (v.size() == 13) {
            // attached: the element it's attached to (role, player, group).
            bool to_ok = false;
            for (int k = 0; k < (int)Role::Count; k++) {
                if (v[10] == std::string("Role::") + role_name((Role)k)) {
                    o.to_role = (Role)k;
                    to_ok = true;
                }
            }
            if (!to_ok) {
                error = path + ":" + std::to_string(n) + ": unknown role it's attached to";
                return false;
            }
            o.to_player = (int8_t)std::strtol(v[11].c_str(), nullptr, 0);
            o.to_group = (uint8_t)std::strtol(v[12].c_str(), nullptr, 0);
        }
        if (v.size() >= 10) {
            bool mode_ok = false;
            for (int k = 0; k < (int)HudMode::Count; k++) {
                std::string name = mode_name((HudMode)k);
                // HudMode::Race, ::RaceBattle, ...: the enumerator's name from its snake_case one.
                std::string e = "HudMode::";
                bool up = true;
                for (char ch : name) {
                    if (ch == '_') { up = true; continue; }
                    e += up ? (char)std::toupper((unsigned char)ch) : ch;
                    up = false;
                }
                if (v[9] == e) {
                    o.mode = (HudMode)k;
                    mode_ok = true;
                }
            }
            if (!mode_ok) {
                error = path + ":" + std::to_string(n) + ": unknown mode";
                return false;
            }
        }
        list.push_back(o);
    }
    return true;
}

// The ROM's HUD images as PNGs (the editor draws with them).
int dump_images(const hud_rom::Rom& rom, const std::string& dir) {
    fs::create_directories(dir);
    int n = 0;
    for (int lang = 0; lang < 3; lang++) {
        for (const auto& [name, im] : rom.images[lang]) {
            if (lang > 0) {
                auto en = rom.images[0].find(name);
                if (en != rom.images[0].end() && en->second.rgba == im.rgba) continue;
            }
            std::string path = dir + "/" + name + (lang > 0 ? "." + std::to_string(lang) : std::string()) + ".png";
            if (stbi_write_png(path.c_str(), im.w, im.h, 4, im.rgba.data(), im.w * 4)) n++;
        }
    }
    for (const auto& [name, im] : rom.images2049) {
        std::string path = dir + "/2049_" + name + ".png";
        if (stbi_write_png(path.c_str(), im.w, im.h, 4, im.rgba.data(), im.w * 4)) n++;
    }
    std::printf("wrote %d images to %s\n", n, dir.c_str());
    return 0;
}

void list_parts(const Output& out) {
    for (size_t i = 0; i < out.parts.size(); i++) {
        const Part& p = out.parts[i];
        const Place& pl = out.places[i];
        std::printf("%3zu %-10s %-11s P%-2d %-12s at (%d,%d)-(%d,%d)", i, p.kind == Kind::Fill ? "fill" : p.kind == Kind::Text ? "text" : "image",
                    role_name(p.role), p.player + 1, p.texture, p.at.x0, p.at.y0, p.at.x1, p.at.y1);
        if (!(p.reach == p.at)) std::printf(" reach (%d,%d)-(%d,%d)", p.reach.x0, p.reach.y0, p.reach.x1, p.reach.y1);
        if (!p.present) std::printf(" [never shows]");
        if (pl.placed) std::printf(" -> d(%d,%d) origin %X%s", pl.dx, pl.dy, pl.origin, pl.scale != 0 ? " scaled" : "");
        std::printf("  %s\n", p.note);
    }
}

// ---- The matrix ----
struct Variant {
    std::string name;
    std::vector<std::pair<std::string, std::string>> set;
};

std::vector<Variant> variants() {
    std::vector<Variant> v;
    struct Mode {
        const char* name;
        std::vector<std::pair<std::string, std::string>> set;
    };
    const Mode modes[] = {
        { "race", { { "mode", "race" } } },
        { "stunts", { { "mode", "race" }, { "stunts", "1" } } },
        { "deaths", { { "mode", "race" }, { "deaths", "1" } } },
        { "arena", { { "mode", "stunt" }, { "arena", "1" }, { "it", "1" } } },
        { "battle", { { "mode", "battle" }, { "battle_time", "1" } } },
        { "race_battle", { { "mode", "race_battle" } } },
    };
    struct Layout {
        int players;
        const char* layout;
        const char* name;
    };
    const Layout layouts[] = { { 1, "single", "p1" }, { 2, "stacked", "p2_stacked" }, { 2, "side", "p2_side" },
                               { 3, "quads", "p3" }, { 4, "quads", "p4" } };
    for (const Layout& l : layouts) {
        for (const char* aspect : { "4:3", "16:9", "21:9" }) {
            for (int odo = 0; odo < 2; odo++) {
                for (int manual = 0; manual < 2; manual++) {
                    for (const Mode& m : modes) {
                        Variant x;
                        x.name = std::string(l.name) + "_" + (std::strcmp(aspect, "4:3") == 0 ? "4x3" : std::strcmp(aspect, "16:9") == 0 ? "16x9" : "21x9") +
                                 "_" + m.name + (odo ? "_odo" : "") + (manual ? "_manual" : "");
                        x.set = { { "players", std::to_string(l.players) }, { "layout", l.layout }, { "aspect", aspect },
                                  { "odometer", odo ? "1" : "0" }, { "manual", manual ? "1111" : "0000" } };
                        x.set.insert(x.set.end(), m.set.begin(), m.set.end());
                        v.push_back(x);
                    }
                }
            }
        }
        // Each HUD setting off (16:9, odometer, manual).
        for (const char* off : { "time", "speed", "position", "radar", "map", "time_left", "tach", "gear" }) {
            Variant x;
            x.name = std::string(l.name) + "_16x9_no_" + off;
            x.set = { { "players", std::to_string(l.players) }, { "layout", l.layout }, { "aspect", "16:9" },
                      { "odometer", "1" }, { "manual", "1111" }, { "deaths", "1" }, { off, "0" } };
            v.push_back(x);
        }
    }
    return v;
}

int run_matrix(const std::string& dir, const hud_rom::Rom& rom, int zoom,
               const std::vector<std::pair<std::string, std::string>>& extra) {
    fs::create_directories(dir);
    std::ofstream index(dir + "/index.txt");
    int total = 0;
    for (const Variant& v : variants()) {
        Config c = default_config();
        std::string layout = "stacked";
        for (const auto& kv : v.set) apply_set(c, layout, kv.first, kv.second);
        for (const auto& kv : extra) apply_set(c, layout, kv.first, kv.second);
        fix_arrangement(c, layout);
        Output out = solve(c);
        std::vector<Issue> issues = check(out);
        int errors, notes;
        std::string text = issues_text(issues, errors, notes);
        total += errors;
        index << v.name << ": " << (errors || notes ? std::to_string(errors) + " errors, " + std::to_string(notes) + " notes" : "ok") << "\n" << text;
        Canvas cv(c.hud_width, zoom);
        draw(out, rom, cv, "all", true, v.name);
        cv.save(dir + "/" + v.name + ".png");
    }
    std::printf("%s: %d errors (%s/index.txt)\n", dir.c_str(), total, dir.c_str());
    return total ? 1 : 0;
}

// ---- Checking a capture of the game ----
Config config_of(const Json& c) {
    Config cf;
    cf.players = c["players"].i() > 0 ? c["players"].i() : c["views"].i();
    cf.arrangement = (Arrangement)c["arrangement"].i();
    cf.hud_width = (float)c["hud_width"].d();
    cf.widescreen = c["widescreen"].i();
    cf.language = c["language"].i();
    cf.odometer = c["odometer"].i();
    cf.odometer_w = c["odometer_size"][0].i();
    cf.odometer_h = c["odometer_size"][1].i();
    cf.odometer_gap = c["odometer_size"][2].i();
    cf.small_scale = (float)c["small_scale"].d();
    cf.stunt_arena = c["stunt_arena"].i();
    cf.race_type = c["race_type"].i();
    cf.battle = c["battle"].i();
    cf.stunts_all_tracks = c["stunts_all_tracks"].i();
    cf.attract = c["attract"].i();
    cf.deaths = c["deaths"].i();
    cf.mode_option = c["mode_option"].i();
    cf.battle_hud = c["battle_hud"].i();
    cf.battle_time_limit = c["battle_time_limit"].i();
    for (int p = 0; p < 4; p++) cf.manual[p] = c["manual"][p].i();
    if (c["shown"].type == Json::Arr) {
        bool* shown[] = { &cf.shown.time, &cf.shown.speed, &cf.shown.position, &cf.shown.radar, &cf.shown.map,
                          &cf.shown.time_left, &cf.shown.tach, &cf.shown.lap_banner, &cf.shown.gear };
        for (int k = 0; k < 9; k++) {
            if (c["shown"][k].type != Json::Null) *shown[k] = c["shown"][k].i();
        }
    }
    return cf;
}

int run_truth(const std::string& dir) {
    std::ifstream f(dir + "/frames.jsonl", std::ios::binary);
    std::string line;
    int frames = 0, widgets = 0;
    std::map<std::string, int> no_part, never, outside, moved_wrong;
    std::map<std::string, std::string> example;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        JsonParser jp{ line.c_str() };
        Json j = jp.parse();
        const Json& cj = j["config"];
        if (cj["attract"].i()) continue;
        Config c = config_of(cj);
        Output out = solve(c);
        frames++;
        for (const Json& w : j["widgets"].arr) {
            if (!w["hud"].i()) continue;
            if ((w["managed"].i() || w["shifted"].i()) && (w["out"]["dx"].i() || w["out"]["dy"].i())) {
                // Another file places it (the battle HUD's bar): the layout must leave it alone.
                char key[64];
                std::snprintf(key, sizeof(key), "slot %d (placed by its own code)", w["slot"].i());
                moved_wrong[key]++;
                continue;
            }
            if (w["managed"].i() || w["shifted"].i() || w["screen_fill"].i()) continue;
            if (w["rect"].type != Json::Arr) continue; // no image (the stunt text's entry: its callback prints)
            const Json& id = w["id"];
            const Json& o = w["out"];
            // Where the game put it, before the layout moved it (a capture since the build identity was recorded
            // has it so; an older one has it moved).
            Rect r{ w["rect"][0].i(), w["rect"][1].i(), w["rect"][2].i(), w["rect"][3].i() };
            if (id["kind"].type == Json::Null) {
                r = Rect{ w["xy"][0].i(), w["xy"][1].i(), w["rect"][2].i(), w["rect"][3].i() };
                if (o["anchored"].i()) r = r.moved(-o["dx"].i(), -o["dy"].i());
            }
            char key[96];
            int pi = -1;
            if (id["kind"].type != Json::Null) {
                // A capture with the identity the widget was made with (src/hud.cpp rush2_hud_entry_created/box_part).
                rush2::hud_catalog::Identity ident;
                ident.kind = (rush2::hud_catalog::Identity::Kind)id["kind"].i();
                std::snprintf(ident.name, sizeof(ident.name), "%s", id["name"].str.c_str());
                ident.x = (int16_t)id["xy"][0].i();
                ident.y = (int16_t)id["xy"][1].i();
                ident.x1 = (int16_t)id["xy"][2].i();
                ident.y1 = (int16_t)id["xy"][3].i();
                ident.callback = id["cb"].hex();
                ident.param = id["p"].hex();
                ident.part = (int8_t)id["part"].i();
                std::snprintf(key, sizeof(key), "%s %08X %08X (%d,%d) part %d", ident.name, ident.callback, ident.param,
                              ident.x, ident.y, ident.part);
                pi = rush2::hud_catalog::find(out.parts, ident);
                if (pi < 0 && ident.kind == rush2::hud_catalog::Identity::Kind::None && !w["shown"].i()) continue;
            }
            else {
                // An older capture (the widget's identity as it was when captured, its parameter maybe changed by its
                // callback): the part with its image and callback (a box's: its callback and side) whose reach holds
                // where it was drawn, preferring one that can appear.
                std::string name = id["name"].str;
                uint32_t cb = id["cb"].hex();
                bool box = id["part"].i() >= 0;  // the box index is 0 for the first box
                int part = id["part"].i();
                if (!w["shown"].i()) continue;
                std::snprintf(key, sizeof(key), "%s %s %08X%s", box ? "box" : "", name.c_str(), cb,
                              box ? (" part " + std::to_string(part)).c_str() : "");
                int any = -1;
                for (size_t k = 0; k < out.parts.size(); k++) {
                    const Part& q = out.parts[k];
                    bool same = box ? q.id.kind == rush2::hud_catalog::Identity::Kind::Box && q.id.callback == cb && q.id.part == part
                                    : q.id.kind == rush2::hud_catalog::Identity::Kind::Entry && q.id.callback == cb && name == q.id.name;
                    if (!same) continue;
                    if (any < 0) any = (int)k;
                    if (q.reach.contains(r) || q.kind == Kind::Text) {
                        if (pi < 0 || (!out.parts[pi].present && q.present)) pi = (int)k;
                    }
                }
                if (pi < 0) pi = any;
            }
            widgets++;
            if (pi < 0) {
                no_part[key]++;
                continue;
            }
            const Part& p = out.parts[pi];
            if (!w["shown"].i()) continue;
            int ei = out.places[pi].element;
            if (!p.present || ei < 0 || !out.elements[ei].possible) {
                // The engine keeps no room for it: something could be placed over it.
                never[key]++;
                char b[128];
                std::snprintf(b, sizeof(b), "drawn (%d,%d)-(%d,%d); %s", r.x0, r.y0, r.x1, r.y1,
                              !p.present ? "the catalog says it never shows" : "the engine says it can't appear (possible())");
                example[std::string("n ") + key] = b;
            }
            if (p.kind != Kind::Text && !p.reach.contains(r)) {
                outside[key]++;
                char b[160];
                std::snprintf(b, sizeof(b), "drawn (%d,%d)-(%d,%d), reach (%d,%d)-(%d,%d)", r.x0, r.y0, r.x1, r.y1, p.reach.x0,
                              p.reach.y0, p.reach.x1, p.reach.y1);
                example[std::string("o ") + key] = b;
            }
            if (id["kind"].type == Json::Null) continue; // an older capture's moves are an older layout's
            const Place& pl = out.places[pi];
            bool anchored = o["anchored"].i();
            int dx = o["dx"].i(), dy = o["dy"].i();
            uint16_t origin = (uint16_t)o["origin"].i();
            uint16_t want = pl.placed ? pl.origin : origin_none;
            if (dx != pl.dx || dy != pl.dy || (anchored && origin != want)) {
                moved_wrong[key]++;
                char b[160];
                std::snprintf(b, sizeof(b), "game d(%d,%d) origin %X, engine d(%d,%d) origin %X", dx, dy, origin, pl.dx, pl.dy, want);
                example[std::string("m ") + key] = b;
            }
        }
    }
    std::printf("%s: %d frames, %d widgets\n", dir.c_str(), frames, widgets);
    auto report = [&](const char* what, const std::map<std::string, int>& m, const char* ex) {
        std::printf("  %s: %zu\n", what, m.size());
        for (const auto& kv : m) {
            auto it = example.find(std::string(ex) + " " + kv.first);
            std::printf("    %s  x%d%s%s\n", kv.first.c_str(), kv.second, it != example.end() ? "  " : "",
                        it != example.end() ? it->second.c_str() : "");
        }
    };
    report("widgets with no catalog part", no_part, "-");
    report("drawn, but their part can't appear in this race", never, "n");
    report("drawn outside their part's reach", outside, "o");
    report("moved other than the engine says", moved_wrong, "m");
    return no_part.empty() && never.empty() && outside.empty() && moved_wrong.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string rom_path = "rush2.us.recomp.z64", out_path = "tmp/hud_preview/preview.png", json_path, show = "all", matrix, truth;
    std::string overrides_path, images_dir, rom2049_path;
    int zoom = 3;
    bool parts = false, picture = true;
    std::vector<std::pair<std::string, std::string>> sets;
    for (int a = 1; a < argc; a++) {
        std::string arg = argv[a];
        // An option's value; another option in its place is a mistake (--matrix --no-picture made a folder named
        // --no-picture).
        auto next = [&]() {
            std::string v = a + 1 < argc ? std::string(argv[++a]) : std::string();
            if (v.rfind("--", 0) == 0) {
                std::fprintf(stderr, "%s needs a value, not %s\n", arg.c_str(), v.c_str());
                std::exit(2);
            }
            return v;
        };
        if (arg == "--rom") rom_path = next();
        else if (arg == "--set") {
            std::string kv = next();
            size_t eq = kv.find('=');
            sets.emplace_back(kv.substr(0, eq), eq == std::string::npos ? "" : kv.substr(eq + 1));
        }
        else if (arg == "--show") show = next();
        else if (arg == "--out") out_path = next();
        else if (arg == "--zoom") zoom = std::max(1, std::atoi(next().c_str()));
        else if (arg == "--json") json_path = next();
        else if (arg == "--matrix") matrix = next();
        else if (arg == "--truth") truth = next();
        else if (arg == "--parts") parts = true;
        else if (arg == "--overrides") overrides_path = next();
        else if (arg == "--no-picture") picture = false;
        else if (arg == "--dump-images") images_dir = next();
        else if (arg == "--rom2049") rom2049_path = next();
        else {
            std::fprintf(stderr, "unknown option %s (see the comment at the top of tools/hud_preview/hud_preview.cpp)\n", arg.c_str());
            return 2;
        }
    }
    if (!truth.empty()) return run_truth(truth);
    hud_rom::Rom rom;
    std::string error;
    if (!rom.load(rom_path, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    {
        // Rush 2049's ROM for the odometer and battle bar images (the game's copy; RUSH2049_ROM or --rom2049).
        const char* env = std::getenv("RUSH2049_ROM");
        std::string path2049 = !rom2049_path.empty() ? rom2049_path : env != nullptr ? env : "build/rush2049.z64";
        if (!rom.load2049(path2049)) {
            std::fprintf(stderr, "note: no Rush 2049 images (%s): the odometer and battle bar are drawn as shapes\n", path2049.c_str());
        }
    }
    if (!matrix.empty()) return run_matrix(matrix, rom, zoom, sets);
    if (!images_dir.empty()) return dump_images(rom, images_dir);
    std::vector<Override> overrides;
    if (!overrides_path.empty() && !read_overrides(overrides_path, overrides, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    Config c = default_config();
    std::string layout = "stacked";
    for (const auto& kv : sets) {
        if (!apply_set(c, layout, kv.first, kv.second)) {
            std::fprintf(stderr, "unknown setting %s\n", kv.first.c_str());
            return 2;
        }
    }
    fix_arrangement(c, layout);
    if (!overrides_path.empty()) c.overrides = &overrides;
    Output out = solve(c);
    std::vector<Issue> issues = check(out);
    if (parts) list_parts(out);
    for (const Element& e : out.elements) {
        if (!e.possible) continue;
        std::printf("  %-16s (%d,%d)-(%d,%d) origin %03X scale %.2f  %s [place %d]\n", element_name(e).c_str(), e.box.x0,
                    e.box.y0, e.box.x1, e.box.y1, e.origin, e.scale, e.rule, e.spot + 1);
    }
    int errors, notes;
    std::printf("%s", issues_text(issues, errors, notes).c_str());
    if (picture) {
        fs::path op(out_path);
        if (op.has_parent_path()) fs::create_directories(op.parent_path());
        Canvas cv(c.hud_width, zoom);
        draw(out, rom, cv, show, true, out_path);
        if (!cv.save(out_path)) std::fprintf(stderr, "can't write %s\n", out_path.c_str());
        else std::printf("wrote %s: %d errors, %d notes\n", out_path.c_str(), errors, notes);
    }
    if (!json_path.empty()) std::ofstream(json_path, std::ios::binary) << elements_json(out, issues, &rom);
    return errors ? 1 : 0;
}
