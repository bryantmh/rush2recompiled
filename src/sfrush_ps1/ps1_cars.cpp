// Converts the PlayStation SF Rush's own cars into new cars (docs/car_mods.md#new-cars; formats and the PS1 code they
// come from: docs/sfrush_ps1.md#cars). They are SF Rush cars: the game filter files them under SF Rush and they list
// after Rush 2's cars (src/car_mods.cpp tags them by this mod's id), their names in SF Rush's lettering (logos from
// tools/build_banners.py --ps1). Each is its PSV model at the size the PS1 loads it (car_load's scale per model) times
// car_size, in the model dump's space (1/16 ft, y up, +z the front, x negated; the PSV's front is -x), with its own
// texture drawn through the player's paint page as the PS1 draws it, and the shared underside texture
// (EXTRA/WHEELS.GFX, face texture 2). Its wheels go where the PS1 puts them (func_8004B230: its base car's offsets plus
// its own tweaks) at the PS1 wheel's size (COLLDAT3/WHEEL.PSV); the UFO draws none, as on the PS1 ("no_wheels"). Its
// handling is the PS1's own (docs/sfrush_ps1.md#car-physics): the PS1 builds each special car on one of SF Rush's eight
// cars, whose row of the physics table (0x800AA4D4) and class set its speed, steering, grip and weight, mapped onto
// Rush 2's roster as the Rush 2049 cars' values are (ps1_physics).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>

#include "sfrush_ps1.h"
#include "../arcade2049/arcade_internal.h"

namespace ps1 = rush2::sfrush_ps1;
namespace a49 = rush2::arcade2049;
using namespace a49;   // V3 and its operators

namespace {
    struct Car {
        const char* id;       // rush2/new_cars/<id>
        const char* name;     // the car select's name
        const char* model;
        const char* texture;
        int special;          // the PS1's special car number (gp+0x28, 0x800B26F8)
        int ps1_type;         // the SF Rush car it is built on (func_800142C4 sets it): its physics row and wheels
        int scale;            // car_load's model scale (func_8003E714 a1): coordinate x scale / 640 = ft
        bool paintable;       // takes the player's paint (all but PIGGY, which the PS1 always paints white)
    };
    // SF Rush's cars, by PS1 car type (car +0x4C): 0 BMW, 1 BUS, 2 CAM, 3 VIPER, 4 SUPRA, 5 BUG, 6 BUGETTI, 7 CONCEPT.
    const Car cars[] = {
        { "PS1STARSKY", "STARSKY", "CARS/STARSKY.PSV", "CARSGFX/HUTCH01.GFX", 1, 6, 12, true },
        { "PS1JEEP", "JEEP", "CARS/JEEP.PSV", "CARSGFX/JEP01.GFX", 2, 5, 12, true },
        { "PS1JETCAR", "JET CAR", "CARS/JETCAR.PSV", "CARSGFX/JET01.GFX", 3, 3, 12, true },
        { "PS1MINI", "MINI", "CARS/MINI.PSV", "CARSGFX/MINI01.GFX", 4, 4, 13, true },
        { "PS1BUS", "CITY BUS", "CARS/BBUS.PSV", "CARSGFX/BBUS.GFX", 5, 1, 13, true },
        { "PS1POLICE", "POLICE", "CARS/PIGGY.PSV", "CARSGFX/PIGGY14.GFX", 7, 2, 12, false },
        { "PS1UFO", "UFO", "EXTRA/UFO.PSV", "EXTRA/UFO.GFX", 8, 7, 19, true },
    };
    constexpr int special_ufo = 8;
    // The PS1 player's paint starts at 0, BLUE (0x800B2FE8); Rush 2's nearest is color 10 (0x0000F0, its ROM table
    // 0x800CE19C) against BLUE.GFX's brightest ramp entry 0x0850A8.
    constexpr int default_main_color = 10;
    constexpr double rush2_wheel_radius = 15.0;   // Rush 2's wheel model at scale 1, 1/16 ft (docs/arcade2049.md#cars)
    // Rush 2's axle height in a car model (1/16 ft): every stock car's wheels sit at y 3.2 (front) and 4.0 (rear), its
    // body from y 0 up (docs/car_mods.md#new-cars). The physics takes the model's origin as the center of mass, so a
    // PS1 model, centered on its box (its origin half its height over its underside), is lifted to put its axles'
    // mean here: centered, the police car's mass sat 2.4 ft over a Rush 2 car's, and it went over backwards under
    // throttle.
    constexpr double rush2_axle_y = (3.2 + 4.0) / 2.0;
    // The PS1 draws its cars bigger than the N64 game: SF Rush's eight cars are in both (Rush 2's types 4-11 are the N64
    // SF Rush's models), and each PS1 model at car_load's scale is 1 / 0.818 times as long as the same car in Rush 2
    // (0.756 the bus to 0.855 the Viper; `python tools/sfrush_ps1/car.py size`). The PS1's own cars are drawn at that
    // scale, wheels with them: the PS1 wheel (1.14 ft) then comes to 0.93 ft, Rush 2's wheel (0.94 ft) to 1%.
    constexpr double car_size = 0.8184;
    // Rush 2's range across and up: the widest of its cars (all but the secret ROCKET, as ps1_physics's roster), the
    // CADDY at 6.62 ft, and the tallest, the DEW at 5.12 (each intact model's extent, tools/car_mod.py rush2_car). At
    // car_size the Starsky and Police are 7.0 ft wide and the UFO 10.2 ft across, so each car is shrunk further,
    // wheels with it, by as much as its width or height needs to fit; the others keep car_size. Length is free: the
    // City Bus keeps its 18.7 ft (a bus is long; the user's call, against fitting it to the CADDY's 15.0).
    constexpr double rush2_max_width = 6.625, rush2_max_height = 5.125;

    // RUSH.EXE tables (docs/sfrush_ps1.md#car-physics, #wheels).
    constexpr uint32_t physics_rows = 0x800AA4D4;  // 44 bytes per car type, func_8005B340
    constexpr uint32_t wheel_rows = 0x800A9CD0;    // 4 s8 per car type, func_8004B230
    constexpr int wheel_scale_ps1 = 12;            // wheel_model_load (0x8004B120) loads COLLDAT3/WHEEL.PSV at 12
    // func_8005D190: a car moves its velocity x this / 128 a frame, by class (car type / 2).
    constexpr int class_step[4] = { 0x80, 0x84, 0x88, 0x94 };
    // func_8008C038: the push a car gives in a collision, x this / 128 by its class.
    constexpr int class_push[4] = { 192, 200, 208, 218 };
    // func_80064D48: a hit damages the car when its speed / this is 7 or more (types 6 and 7: 9, the rest 10).
    int damage_divisor(int type) { return type < 6 ? 10 : 9; }

    // The new car format's base (engine sound and what every Rush 2 car shares; ps1_physics sets the rest).
    const char* reference = "PICKUP";


    using Face = ps1::PsvFace;
    bool read_psv(const std::vector<uint8_t>& d, std::vector<V3>& verts, std::vector<Face>& faces) {
        return ps1::read_psv(d, verts, faces);
    }

    // A GFX file: 8 bytes, a 16-color CLUT, 4-bit texels 256 wide. 0x0000 is transparent. clut: the CLUT to draw with
    // (car_load uploads only a car texture's texels, so the PS1 draws it with the paint page's CLUT; nullptr: the
    // file's own). mask: when given, a paint mask (MAIN, red) over the paint ramp, CLUT entries 1-6.
    bool gfx(const std::vector<uint8_t>& d, std::vector<uint8_t>& rgba, int& w, int& h, bool& clear,
             const uint8_t* clut = nullptr, std::vector<uint8_t>* mask = nullptr) {
        if (d.size() < 40 + 128) return false;
        if (!clut) clut = &d[8];
        w = 256;
        h = int((d.size() - 40) * 2 / 256);
        rgba.assign(size_t(w) * h * 4, 0);
        if (mask) mask->assign(size_t(w) * h * 4, 0);
        clear = false;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int i = (d[40 + (size_t(y) * w + x) / 2] >> ((x & 1) * 4)) & 15;
                uint16_t c = uint16_t(clut[2 * i] | clut[2 * i + 1] << 8);
                if (mask && i >= 1 && i <= 6) {
                    uint8_t* m = &(*mask)[(size_t(y) * w + x) * 4];
                    m[0] = 255, m[3] = 255;
                }
                if (mask) (*mask)[(size_t(y) * w + x) * 4 + 3] = 255;
                uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
                uint8_t r = uint8_t((c & 31) << 3), g = uint8_t(((c >> 5) & 31) << 3), b = uint8_t(((c >> 10) & 31) << 3);
                p[0] = uint8_t(r | r >> 5), p[1] = uint8_t(g | g >> 5), p[2] = uint8_t(b | b >> 5), p[3] = c ? 255 : 0;
                clear = clear || c == 0;
            }
        }
        return true;
    }

    // PS1 car space (-x the front, y across, z up) at the model's scale to the model dump's (x negated, y up, +z the
    // front; 1/16 ft): coordinate x scale / 640 ft (func_8003E714 stores coordinate x a1 / 64 in 0.1 ft), so x scale / 40
    // in 1/16 ft. PSV models are mirrored against the track's frame: so mapped, the lettering on the cars (the Mini's
    // 15, MAYLEE, CURTIS) reads the right way round on both sides, and the grille leads. size: the car's size against
    // the PS1's (car_size, times its fit to Rush 2's range).
    V3 dump(const V3& p, int scale, double size) {
        double k = scale / 40.0 * size;
        return { -p[1] * k, p[2] * k, -p[0] * k };
    }

    // Where the PS1 draws the right front and rear wheels (func_8004B230), in the model dump's space: its base car's
    // offsets (0x800A9CD0: s8 x4, 0.1 ft) around 30 ft/10 across and 51 ft/10 ahead and behind, the special car's own
    // tweaks, 0.9 ft under the car's origin (the physics' +9, y down). Left and right differ by a little on some cars:
    // Rush 2 mirrors one, so it is their mean. All in the PS1's car units, so at the car's size too.
    void ps1_wheels(const ps1::Exe& exe, const Car& car, double size, V3& front, V3& rear) {
        uint32_t row = wheel_rows + 4 * uint32_t(car.ps1_type);
        int b0 = exe.b(row), b1 = exe.b(row + 1), b2 = exe.b(row + 2), b3 = exe.b(row + 3);
        int left = -30 - b0, right = 30 + b2, ahead = 51 + b1, behind = -51 - b3;
        switch (car.special) {
            case 1: right = 35 + b2, behind -= 8, left -= 5, ahead += 13; break;   // STARSKY
            case 2: ahead += 6; break;                                             // JEEP
            case 3: ahead -= 7, behind -= 7; break;                                // JETCAR
            case 4: ahead = 41 + b1; break;                                        // MINI
            case 5: left -= 6, right += 4, behind -= 1, ahead += 10; break;        // BBUS
            case 7: right += 4, left -= 4, behind -= 7, ahead += 15; break;        // PIGGY
            default: break;                                                        // 8 UFO: its base car's
        }
        const double to16 = 1.6 * size;   // 0.1 ft to 1/16 ft
        double across = (right - left) / 2.0 * to16, down = -9 * to16;
        front = { across, down, ahead * to16 };
        rear = { across, down, behind * to16 };
    }

    // The height of a wheel arch's top over z = `at` (model dump space): the side silhouette's lowest filled point per
    // column (every face but the underside's), highest within a third of the wheel's radius of `at`. The PS1 draws
    // its wheels on the ground under each wheel point (the suspension heights 0x800E9938, from 9), so their height on
    // the body is where the body's arches put them.
    double arch_top(const std::vector<V3>& V, const std::vector<Face>& F, int scale, double size, double at, double radius) {
        std::map<int, double> bottom;   // column (1/16 ft) -> lowest y
        for (const Face& f : F) {
            if (f.tex == 2) continue;
            for (size_t a = 1; a + 1 < f.idx.size(); a++) {
                V3 p[3] = { dump(V[f.idx[0]], scale, size), dump(V[f.idx[a]], scale, size), dump(V[f.idx[a + 1]], scale, size) };
                double z0 = std::min({ p[0][2], p[1][2], p[2][2] }), z1 = std::max({ p[0][2], p[1][2], p[2][2] });
                for (int z = int(std::ceil(z0)); z <= int(std::floor(z1)); z++) {
                    // The triangle's lowest point on this column: its edges crossing z.
                    for (int e = 0; e < 3; e++) {
                        const V3& u = p[e];
                        const V3& v = p[(e + 1) % 3];
                        if ((u[2] - z) * (v[2] - z) > 0 || u[2] == v[2]) continue;
                        double y = u[1] + (z - u[2]) / (v[2] - u[2]) * (v[1] - u[1]);
                        auto it = bottom.find(z);
                        if (it == bottom.end() || y < it->second) bottom[z] = y;
                    }
                }
            }
        }
        double top = -1e9;
        for (int z = int(std::floor(at - radius / 3)); z <= int(std::ceil(at + radius / 3)); z++) {
            auto it = bottom.find(z);
            if (it != bottom.end()) top = std::max(top, it->second);
        }
        return top;
    }

    // The car as a glb: one material per texture, smooth normals (Rush 2 bakes its light into vertex colors from them).
    std::vector<uint8_t> convert(const ps1::Disc& disc, const Car& car, const std::vector<V3>& V, const std::vector<Face>& F,
                                 double size, double lift, size_t& tri_count, std::string& error) {
        a49::Glb g("rush2-recomp src/sfrush_ps1 (PS1 SF Rush car)", false);
        struct Tex {
            int material = -1, w = 0, h = 0;
        };
        std::map<int, Tex> texs;   // 0: the car's own, 2: WHEELS.GFX
        // The paint: the PS1 draws the car with the player's paint page (CARSGFX/<color>.GFX, func_80030930); PIGGY's
        // is always WHITE (func_800142C4), the others take any color. The car's texels on the paint ramp get WHITE's
        // neutral ramp (their brightness), and the paintable cars a mask over them, so Rush 2 paints them in any of its
        // colors.
        std::vector<uint8_t> white;
        if (!disc.read("CARSGFX/WHITE.GFX", white) || white.size() < 40) {
            error = "CARSGFX/WHITE.GFX couldn't be read";
            return {};
        }
        // A painted car's ramp is WHITE's with the PS1's full paint at Rush 2's: Rush 2 draws its car palette's MAIN ramp
        // as the paint x the entry's grey (231 to 0: red's entry 1 is (214, 0, 0), its grey 107 (99, 0, 0)), and a mod
        // car's texel takes the ramp entry of its brightness x 230 / 255 (the stand-in; the replacement draws the
        // ramp's lightest entry x grey / 230, the same). So entry 6, the PS1 paint page's full color (BLUE's 0x0850A8
        // is Rush 2's color 10), goes to 230, the ramp's top, and the rest keep the PS1's steps under it: entry 4, most
        // of a body, at 150, 65% of the paint as on the PS1's pages. (Entry 6 at 107 left bodies at 70: dark brown in
        // red.)
        std::vector<uint8_t> ramp(white.begin() + 8, white.begin() + 40);
        if (car.paintable) {
            auto level = [&](int i) { return ((ramp[2 * i] | ramp[2 * i + 1] << 8) & 31) << 3; };
            double k = 230.0 / std::max(1, level(6));
            for (int i = 1; i <= 6; i++) {
                int v = std::clamp(int(std::lround(level(i) * k)) >> 3, 1, 31);
                uint16_t c = uint16_t(v | v << 5 | v << 10);
                ramp[2 * i] = uint8_t(c), ramp[2 * i + 1] = uint8_t(c >> 8);
            }
        }
        for (int which : { 0, 2 }) {
            std::vector<uint8_t> d, rgba, mask;
            Tex t;
            bool clear = false;
            const char* file = which == 2 ? "EXTRA/WHEELS.GFX" : car.texture;
            bool painted = which == 0 && car.paintable;
            if (!disc.read(file, d) || !gfx(d, rgba, t.w, t.h, clear, which == 0 ? ramp.data() : nullptr, painted ? &mask : nullptr)) {
                error = std::string(file) + " couldn't be read";
                return {};
            }
            int image = g.image(file, a49::png(rgba, t.w, t.h));
            t.material = g.material(which == 2 ? "PS1_UNDERSIDE" : car.id, image, clear ? "MASK" : "OPAQUE", false, { 33071, 33071 });
            if (painted) g.paint_mask(t.material, g.image(std::string(file) + " paint", a49::png(mask, t.w, t.h)));
            texs[which] = t;
        }
        std::vector<a49::Glb::Prim> prims;
        tri_count = 0;
        for (int which : { 0, 2 }) {
            const Tex& t = texs[which];
            a49::Glb::Prim p;
            p.material = t.material;
            for (const Face& f : F) {
                if ((f.tex == 2) != (which == 2)) continue;
                std::array<double, 2> centre{};
                for (const auto& uv : f.uv) centre[0] += uv[0] / f.uv.size(), centre[1] += uv[1] / f.uv.size();
                for (size_t a = 1; a + 1 < f.idx.size(); a++) {
                    // Wound as the track's faces are (ps1_track.cpp fan).
                    for (size_t k : { size_t(0), a, a + 1 }) {
                        V3 q = dump(V[f.idx[k]], car.scale, size);
                        q[1] += lift;
                        p.verts.push_back(q);
                        std::array<double, 2> uv = ps1::texel_inset(f.uv[k], centre);
                        p.uvs.push_back({ uv[0] / t.w, uv[1] / t.h });
                    }
                }
            }
            if (p.verts.empty()) continue;
            p.normals = a49::smooth_normals(p.verts);
            for (uint32_t k = 0; k < p.verts.size(); k += 3) p.tris.push_back({ k, k + 1, k + 2 });
            tri_count += p.tris.size();
            prims.push_back(std::move(p));
        }
        g.textured_mesh(car.id, prims);
        return g.bytes();
    }
}

// The PS1 handling in Rush 2's values (docs/sfrush_ps1.md#car-physics), mapped onto Rush 2's roster as the Rush 2049
// cars' are (src/rush2049/car2049.cpp, Car speeds): a value the PS1 sets per car goes to the Rush 2 value that does its
// job, times Rush 2's mean over the PS1's (its eight cars, the physics table's rows), so each car keeps its place among
// SF Rush's and the roster's middle lands on Rush 2's; a value the PS1 has no counterpart for is Rush 2's mean (its 21
// cars, all but the secret ROCKET: the ROM's descriptors 0x800C07BC and per-type tables, `tools/rush2049/cars.py r2`).
//   - speed: the thrust by gear (+0x8..+0x14) x the class's step (func_8005D190) -> the torque scales for gears 1, 2
//     and 3 up (the PS1's third and fourth gears' mean)
//   - steering rate (+0x4) -> steering
//   - tire grip (+0x1C): the sideways friction is (grip + 64) / 256 at its most (func_8005E34C) -> rear grip
//   - the collision push by class (func_8008C038) -> mass and the collision weight (pitch / roll inertia)
//   - the damage divisor (func_80064D48: a hit at speed / it of 7 or more damages) -> the crash threshold (yaw inertia)
//   - no counterpart: yaw damping, preload, gears, durability, suspension and tires settings, and the UFO's and
//     PIGGY's speed rules (func_8005D190: every force on the UFO doubled under speed 100, PIGGY's cut over 301): they
//     scale grip with the push, which no Rush 2 value does (the UFO's push alone doubled, as torque, pulled it over
//     backwards from a standing start)
// Every car has the same suspension, inertia constants, tire curves, final drive, drag, rolling resistance, clutch,
// drivetrain inertia and shift points in Rush 2 (and the PICKUP base), so those stay.
static nlohmann::json ps1_physics(const ps1::Exe& exe, const Car& car, std::string& note) {
    auto row = [&](int type, int offset) { return int32_t(exe.w(physics_rows + 44 * uint32_t(type) + uint32_t(offset))); };
    // Rush 2's roster means (21 cars).
    constexpr double r2_torque[3] = { 1.82019, 1.84157, 1.88548 }, r2_common_torque = 1.3;
    constexpr double r2_gears[6] = { -3.1, 0.0, 3.09524, 1.75429, 1.25381, 0.95619 };
    constexpr double r2_steering = 542.857, r2_yaw_damping = 30.714, r2_rear_grip = 0.95952, r2_mass = 108.363;
    constexpr double r2_pitch_roll = 2728.57, r2_yaw_inertia = 387619.0, r2_preload = 0.18476, r2_durability = 0.52381;
    constexpr int r2_suspension_setting = 1, r2_tires_setting = 5;   // means 0.905 and 5.0
    // The PS1's values for car type t.
    auto speed = [&](int t, int gear) {   // gear 0, 1, 2 (3 and 4)
        double thrust = gear < 2 ? row(t, 0x8 + 4 * gear) : (row(t, 0x10) + row(t, 0x14)) / 2.0;
        return thrust * class_step[t >> 1];
    };
    auto steer = [&](int t) { return double(row(t, 0x4)); };
    auto grip = [&](int t) { return double(row(t, 0x1C) + 64); };
    auto push = [&](int t) { return double(class_push[t >> 1]); };
    auto damage = [&](int t) { return double(damage_divisor(t)); };
    auto mapped = [&](const std::function<double(int)>& v, double r2_mean) {
        double mean = 0.0;
        for (int t = 0; t < 8; t++) mean += v(t) / 8.0;
        return mean != 0.0 ? v(car.ps1_type) * r2_mean / mean : r2_mean;
    };
    auto round = [](double v, double step) { return std::round(v / step) * step; };
    nlohmann::json torque = nlohmann::json::array();
    for (int g = 0; g < 3; g++) {
        torque.push_back(round(mapped([&](int type) { return speed(type, g); }, r2_torque[g]), 0.0001));
    }
    torque.push_back(r2_common_torque);
    nlohmann::json gears = nlohmann::json::array();
    for (double r : r2_gears) gears.push_back(r);
    nlohmann::json out = {
        { "torque", torque },
        { "gears", gears },
        { "steering", round(mapped(steer, r2_steering), 0.1) },
        { "yaw_damping", r2_yaw_damping },
        { "rear_grip", round(mapped(grip, r2_rear_grip), 0.0001) },
        { "mass", round(mapped(push, r2_mass), 0.001) },
        { "pitch_roll_inertia", round(mapped(push, r2_pitch_roll), 0.1) },
        { "yaw_inertia", round(mapped(damage, r2_yaw_inertia), 1) },
        { "preload", r2_preload },
        { "durability", r2_durability },
        { "suspension_setting", r2_suspension_setting },
        { "tires_setting", r2_tires_setting },
    };
    char text[200];
    snprintf(text, sizeof(text), "SF Rush car %d: torque %.3f %.3f %.3f, steering %.0f, rear grip %.3f, mass %.1f", car.ps1_type,
             double(torque[0]), double(torque[1]), double(torque[2]), double(out["steering"]), double(out["rear_grip"]),
             double(out["mass"]));
    note = text;
    return out;
}

bool ps1::build_cars(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log) {
    char line[320];
    Exe exe;
    if (!exe.open(disc) || !exe.in(physics_rows, 44 * 8) || !exe.in(wheel_rows, 4 * 8) || !exe.in(0x800A8EE8, 12 * 16) ||
        !exe.in(0x800A8FA8, 12 * 16)) {
        log += "EXE/RUSH.EXE couldn't be read\n";
        return false;
    }
    // The PS1 wheel: COLLDAT3/WHEEL.PSV at its scale, its radius the extent across its tread (x and z; y is its axle).
    std::vector<uint8_t> d;
    std::vector<V3> wheel;
    std::vector<Face> wheel_faces;
    if (!disc.read("COLLDAT3/WHEEL.PSV", d) || !read_psv(d, wheel, wheel_faces) || wheel.empty()) {
        log += "COLLDAT3/WHEEL.PSV couldn't be read\n";
        return false;
    }
    double reach = 0;
    for (const V3& v : wheel) reach = std::max({ reach, std::fabs(v[0]), std::fabs(v[2]) });
    double wheel_scale = reach * wheel_scale_ps1 / 40.0 * car_size / rush2_wheel_radius;   // 1/16 ft against Rush 2's wheel
    double ride_sum = 0.0;   // ft at car_size, over the wheeled cars (the UFO is last in `cars`)
    int ride_count = 0;
    for (const Car& car : cars) {
        std::vector<V3> V;
        std::vector<Face> F;
        if (!disc.read(car.model, d) || !read_psv(d, V, F) || V.empty()) {
            log += std::string(car.model) + " couldn't be read\n";
            return false;
        }
        // Its size: car_size, shrunk to fit Rush 2's range where its width or height is past it (rush2_max_width).
        double fit = 1.0, half_width_ft = 0.0;
        {
            V3 lo{ 1e9, 1e9, 1e9 }, hi{ -1e9, -1e9, -1e9 };
            for (const V3& v : V) {
                V3 q = dump(v, car.scale, car_size);
                for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], q[i]), hi[i] = std::max(hi[i], q[i]);
            }
            const double max16[2] = { rush2_max_width * 16.0, rush2_max_height * 16.0 };
            for (int i = 0; i < 2; i++) {
                if (hi[i] > lo[i]) fit = std::min(fit, max16[i] / (hi[i] - lo[i]));
            }
            half_width_ft = (hi[0] - lo[0]) * fit / 32.0;
        }
        const double size = car_size * fit, car_wheel_scale = wheel_scale * fit;
        V3 front{}, rear{};
        ps1_wheels(exe, car, size, front, rear);
        double r = car_wheel_scale * rush2_wheel_radius;   // 1/16 ft
        if (car.special != special_ufo) {
            for (V3* w : { &front, &rear }) {
                double top = arch_top(V, F, car.scale, size, (*w)[2], r);
                if (top > -1e8) (*w)[1] = top - r;
            }
            // The origin's height over the ground (the wheels' bottoms) at car_size, for the UFO below.
            ride_sum += (r - (front[1] + rear[1]) / 2.0) / 16.0 / (size / car_size);
            ride_count++;
        }
        else if (ride_count > 0) {
            // The UFO has no arches to set its wheels by. The PS1's physics doesn't know a car's model, so it carries
            // every car's origin at the same height over the ground; the wheeled cars' arches put it at their mean
            // (2.8 ft at car_size). At that height its underside clears the road by 0.6 ft as theirs do (from the
            // wheel point 0.9 ft down it sat on the road).
            double ride = ride_sum / ride_count * (size / car_size) * 16.0;
            front[1] = rear[1] = r - ride;
        }
        // The model and its wheels lifted together, so the axles are at Rush 2's height (rush2_axle_y).
        double lift = rush2_axle_y - (front[1] + rear[1]) / 2.0;
        front[1] += lift;
        rear[1] += lift;
        std::string error;
        size_t tris = 0;
        std::vector<uint8_t> glb = convert(disc, car, V, F, size, lift, tris, error);
        if (glb.empty()) {
            log += std::string(car.id) + ": " + error + "\n";
            return false;
        }
        out[std::string("rush2/new_cars/") + car.id + ".glb"] = std::move(glb);
        std::string note;
        nlohmann::json spec = { { "name", car.name }, { "base", reference },
                                { "source", std::string("San Francisco Rush (PlayStation) ") + car.model + ", built by the Games tab" },
                                { "physics", ps1_physics(exe, car, note) } };
        double scale = std::round(car_wheel_scale * 1000) / 1000;
        spec["wheels"] = { { "front", { front[0], front[1], front[2] } }, { "rear", { rear[0], rear[1], rear[2] } } };
        spec["wheel_scale"] = { scale, scale };
        // The UFO's wheels aren't drawn (the PS1's wheel loop skips the special car 8); its wheel points still carry it.
        if (car.special == special_ufo) spec["no_wheels"] = true;
        // A saucer: its shadow a disc its own size (src/mod_car_shadow.cpp), not the box between its hidden wheels.
        if (car.special == special_ufo) spec["round_shadow"] = std::round(half_width_ft * 100.0) / 100.0;
        // Nor does it show wings (they work, air control and all, unseen) or the wheels' marks and dust: it hovers.
        if (car.special == special_ufo) {
            spec["no_wings"] = true;
            spec["no_skid_marks"] = true;
            spec["no_wheel_dust"] = true;
        }
        if (car.paintable) spec["main_color"] = default_main_color;
        // Its name in SF Rush's lettering (tools/build_banners.py --ps1: assets/sfrush_ps1/<id>_name.png, 128 x 32).
        {
            std::ifstream f(assets_dir / (std::string(car.id) + "_name.png"), std::ios::binary);
            if (!f) {
                log += "missing " + (assets_dir / (std::string(car.id) + "_name.png")).string() + "\n";
                return false;
            }
            out[std::string("rush2/new_cars/") + car.id + "_name.png"].assign(std::istreambuf_iterator<char>(f), {});
            spec["logo"] = std::string(car.id) + "_name.png";
        }
        out[std::string("rush2/new_cars/") + car.id + ".json"] = a49::text_bytes(spec.dump(4));
        snprintf(line, sizeof(line), "%s -> %s (%s, %s): %zu triangles, wheels front (%.1f, %.1f, %.1f) rear (%.1f, %.1f, "
                 "%.1f) x %.3f, size %.3f of the PS1's\n", car.model, car.id, car.name, note.c_str(), tris, front[0], front[1],
                 front[2], rear[0], rear[1], rear[2], scale, size);
        log += line;
    }
    return true;
}

bool ps1::build_mod(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log,
                    const N64Lanes& n64, const N64Scene& n64_embarcadero) {
    if (!build_track(disc, assets_dir, out, log, n64, n64_embarcadero) || !build_cars(disc, assets_dir, out, log)) return false;
    // Time Sprint's clock (src/time_sprint.cpp, docs/sfrush_ps1.md#time-sprint): its model, its texture page and the
    // sound a clock plays when taken (sound 3, CHECK, of the race's sound bank), as the disc has them.
    {
        std::vector<uint8_t> psv, page, bank;
        if (disc.read("EXTRA/TIME.PSV", psv) && disc.read("ART/PICKUPS.GFX", page) && disc.read("BANKS/GAMESND.BNK", bank)) {
            out["rush2/time_sprint/time.psv"] = psv;
            out["rush2/time_sprint/pickups.gfx"] = page;
            // GAMESND.BNK: "BANK", u32 count, then per sound name[8], u32 offset (from the end of the table), u32 size
            // of PS1 SPU ADPCM; sorted by name, which is the order func_80020750's sound ids index.
            constexpr int check_sound = 3;
            if (bank.size() >= 8 && std::memcmp(bank.data(), "BANK", 4) == 0) {
                uint32_t count = uint32_t(bank[4]) | uint32_t(bank[5]) << 8 | uint32_t(bank[6]) << 16 | uint32_t(bank[7]) << 24;
                size_t table_end = 8 + size_t(count) * 16, entry = 8 + size_t(check_sound) * 16;
                auto u32 = [&](size_t o) {
                    return uint32_t(bank[o]) | uint32_t(bank[o + 1]) << 8 | uint32_t(bank[o + 2]) << 16 | uint32_t(bank[o + 3]) << 24;
                };
                if (count > uint32_t(check_sound) && table_end <= bank.size()) {
                    size_t at = table_end + u32(entry + 8), size = u32(entry + 12);
                    if (at + size <= bank.size()) {
                        out["rush2/time_sprint/check.adpcm"] = std::vector<uint8_t>(bank.begin() + at, bank.begin() + at + size);
                    }
                }
            }
            log += "Time Sprint: the clock's model, texture and sound\n";
        }
        else {
            log += "Time Sprint: the clock's files couldn't be read\n";
        }
    }
    out["mod.json"] = a49::text_bytes(nlohmann::json{
        { "game_id", "rush2" }, { "id", mod_id }, { "display_name", "SF Rush PlayStation" },
        { "description", "The PlayStation San Francisco Rush's own track (Chinatown, its track 4) and cars (Starsky, Jeep, "
                         "Jet Car, Mini, City Bus, Police and the UFO), built from the disc by the Games tab." },
        { "short_description", "SF Rush PS1 track and cars" }, { "version", "1.0.0" }, { "authors", { "rush2-recomp" } },
        { "minimum_recomp_version", "0.0.0" },
        // Converted from the game's own data, so the converter's authoring warnings don't apply to it.
        { "hide_track_warnings", true } }.dump(4));
    return true;
}
