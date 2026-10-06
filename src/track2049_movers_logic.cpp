// Rush 2049 animated track objects: the path follower and its helpers, ported from 2049's code as pure logic.
//
// 2049 spawns its path objects at race setup (func_800B338C -> func_800B2DF8 -> func_800ABCC8 per spawn node, then
// func_800AB7D8 -> type init func_800C1604) and updates each one once per rendered frame with the frame time
// (0x8002EB94) from its instance callback func_800C0AC0, called by func_800B0868 after the frame's game/physics
// step. An update runs a trigger state machine, steps the follower along its PATH nodes, computes the position
// (func_800C04CC), the rotation (func_800C00E0: quaternion interpolation func_800BFD8C, matrix func_800BFBE8) and
// the node scale, and moves the object's collision group (func_800C0294 / func_800BF838). Everything here keeps
// 2049's single-precision operation order, its quantisation (positions to 1/32 by truncation, collision vertices
// to 1/32 by rounding or truncation, matrices to 1/16384) and its quirks, which are noted where they occur. The
// specification with addresses is docs/rush2049_research/movers.md.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "track2049_movers_logic.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace rush2::track2049::movers {
    namespace {
        uint32_t be32(const uint8_t* p) {
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }

        int16_t be16(const uint8_t* p) {
            return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
        }

        float bef(const uint8_t* p) {
            uint32_t v = be32(p);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        // 2049 chunk directory: word 0 = offset of {char tag[4], u32 offset, u32 count} entries.
        bool find_chunk(const uint8_t* data, size_t size, const char* tag, uint32_t& offset, uint32_t& count) {
            if (size < 4) {
                return false;
            }
            for (size_t d = be32(data); d + 12 <= size; d += 12) {
                bool alpha = true;
                for (int i = 0; i < 4; i++) {
                    char c = (char)data[d + i];
                    alpha = alpha && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
                }
                if (!alpha) {
                    break;
                }
                if (std::memcmp(data + d, tag, 4) == 0) {
                    offset = be32(data + d + 4);
                    count = be32(data + d + 8);
                    return true;
                }
            }
            return false;
        }

        // Model-index constants of the flip-book types (2049 handle table 0x801427C0).
        constexpr int16_t anim_shark = 0x169;   // SHARKG2/G4/G5/G16
        constexpr int16_t anim_f1flag = 0x16D;  // F1FLAGG28..G47
        constexpr float shark_period = 0.25f;
        constexpr float f1flag_period = 0.0425f; // 0x80123E8C / 0x80123E90

        // In-place rotation rates per sign sub-kind (0x80118D70, 12 bytes each; x, y, z in rad/s).
        constexpr float in_place_rates[6][3] = {
            { 0.0f, 3.0f, 0.0f }, { -0.38397247f, 0.0f, 0.0f }, { -0.37524578f, 0.0f, 0.0f },
            { 0.0f, 1.5f, 0.0f }, { 1.5f, 0.0f, 0.0f }, { -1.5f, 0.0f, 0.0f },
        };

        // libultra sinf/cosf polynomial (0x8002D750 / 0x8002D7A0) and constants.
        constexpr double sin_p[5] = { 1.0, -0.16666659550427756, 0.008333066246082155, -0.0001980960290193795,
                                      2.605780637968037e-06 };
        constexpr double inv_pi = 0.3183098861837907;
        constexpr double pi_hi = 3.1415926218032837;
        constexpr double pi_lo = 3.178650954705639e-08;

        uint32_t float_bits(float f) {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            return v;
        }

        // Shared tail of sinf/cosf: x + x^3 * P(x^2), in double.
        double sin_poly(double dx) {
            double xsq = dx * dx;
            double poly = sin_p[4] * xsq;
            poly = poly + sin_p[3];
            poly = poly * xsq;
            poly = poly + sin_p[2];
            poly = poly * xsq;
            poly = sin_p[1] + poly;
            double r = dx * xsq;
            r = r * poly;
            return r + dx;
        }

        // func_800BF780: out = b x a (row-major), each element (b0*a0 + b1*a3) + a6*b2 summed as 2049 does.
        void mat_mul(const float a[9], const float b[9], float out[9]) {
            float r[9];
            for (int i = 0; i < 3; i++) {
                const float* row = b + i * 3;
                for (int j = 0; j < 3; j++) {
                    float p0 = row[0] * a[j];
                    float p1 = row[1] * a[3 + j];
                    float p2 = a[6 + j] * row[2];
                    float s = p0 + p1;
                    r[i * 3 + j] = p2 + s;
                }
            }
            std::memcpy(out, r, sizeof(r));
        }

        // func_8009E820: out = v x m (row vector), each component (v0*m0 + v1*m3) + m6*v2.
        void vec_mul(const float v[3], const float m[9], float out[3]) {
            float r[3];
            for (int j = 0; j < 3; j++) {
                float p0 = v[0] * m[j];
                float p1 = v[1] * m[3 + j];
                float p2 = m[6 + j] * v[2];
                float s = p0 + p1;
                r[j] = p2 + s;
            }
            std::memcpy(out, r, sizeof(r));
        }

        // VERT decode (func_800C0294 / func_800BF838): (int * 32 + 5-bit fraction) / 32.
        void decode_vert(const int16_t v[4], float out[3]) {
            uint16_t frac = (uint16_t)v[3];
            out[0] = (float)(((int32_t)v[0] << 5) + ((frac & 0x7C00) >> 10)) * 0.03125f;
            out[1] = (float)(((int32_t)v[1] << 5) + ((frac & 0x3E0) >> 5)) * 0.03125f;
            out[2] = (float)(((int32_t)v[2] << 5) + (frac & 0x1F)) * 0.03125f;
        }

        // Rotations of func_800D03DC's three helpers (func_80090E9C about y, func_80090F44 about x,
        // func_8009EA68 about z); angles within +-0.0001 are skipped.
        void rotate_y(float m[9], float a) {
            if (!(a < -0.0001f || 0.0001f < a)) {
                return;
            }
            float s = sinf2049(a);
            float c = cosf2049(a);
            for (int i = 0; i < 3; i++) {
                float p = m[6 + i];
                float q = m[i];
                float t0 = p * s;
                float t1 = q * c;
                float t2 = p * c;
                float t3 = q * s;
                m[i] = t0 + t1;
                m[6 + i] = t2 - t3;
            }
        }

        void rotate_x(float m[9], float a) {
            if (!(a < -0.0001f || 0.0001f < a)) {
                return;
            }
            float s = sinf2049(a);
            float c = cosf2049(a);
            for (int i = 0; i < 3; i++) {
                float p = m[3 + i];
                float q = m[6 + i];
                float t0 = p * c;
                float t1 = q * s;
                float t2 = p * s;
                float t3 = q * c;
                m[3 + i] = t0 - t1;
                m[6 + i] = t2 + t3;
            }
        }

        void rotate_z(float m[9], float a) {
            if (!(a < -0.0001f || 0.0001f < a)) {
                return;
            }
            float s = sinf2049(a);
            float c = cosf2049(a);
            for (int i = 0; i < 3; i++) {
                float p = m[i];
                float q = m[3 + i];
                float t0 = p * c;
                float t1 = q * s;
                float t2 = p * s;
                float t3 = q * c;
                m[i] = t0 - t1;
                m[3 + i] = t2 + t3;
            }
        }
    }

    // ---- Math -------------------------------------------------------------------------------------------------

    // libultra sinf (2049 0x80008730).
    float sinf2049(float x) {
        int xpt = (int)((float_bits(x) >> 22) & 0x1FF);
        if (xpt < 0xFF) {
            if (xpt >= 0xE6) {
                return (float)sin_poly((double)x);
            }
            return x;
        }
        if (xpt < 0x136) {
            double dx = (double)x;
            double dn = dx * inv_pi;
            int n = dn >= 0.0 ? (int)(dn + 0.5) : (int)(dn - 0.5);
            dn = (double)n;
            dx = dx - dn * pi_hi;
            dx = dx - dn * pi_lo;
            float r = (float)sin_poly(dx);
            return (n & 1) ? -r : r;
        }
        return x == x ? 0.0f : x;
    }

    // libultra cosf (2049 0x800088F0).
    float cosf2049(float x) {
        int xpt = (int)((float_bits(x) >> 22) & 0x1FF);
        if (xpt < 0x136) {
            float ax = 0.0f < x ? x : -x;
            double dx = (double)ax;
            double dn = dx * inv_pi + 0.5;
            int n = 0.0 <= dn ? (int)(dn + 0.5) : (int)(dn - 0.5);
            dn = (double)n - 0.5;
            dx = dx - dn * pi_hi;
            dx = dx - dn * pi_lo;
            float r = (float)sin_poly(dx);
            return (n & 1) ? -r : r;
        }
        return x == x ? 0.0f : x;
    }

    // func_8009C3F8 with a0 = 1 (acos): Cephes-style rational asin, then the quadrant tables 0x8011F010/0x8011F018.
    float acosf2049(float x) {
        constexpr float p[5] = { -0.69674575f, 10.152522f, -39.688862f, 57.20823f, -27.368494f };
        constexpr float q[5] = { -23.823858f, 150.95271f, -381.86304f, 417.14432f, -164.21097f };
        constexpr float table_pos[2] = { 0.0f, 0.78539819f };
        constexpr float table_neg[2] = { 1.5707964f, 0.78539819f };
        float ax = std::fabs(x);
        float r;
        int index = 1;
        if (ax < 2.3e-10f) {
            r = ax;
        }
        else if (1.0f <= ax) {
            r = 1.5707964f;
        }
        else {
            float z;
            float w;
            if (0.5f < ax) {
                float h = 0.5f - ax;
                h = h + 0.5f;
                z = h * 0.5f;
                float s = std::sqrt(z);
                w = s + s;
                w = -w;
                index = 0;
            }
            else {
                z = ax * ax;
                w = ax;
            }
            float num = p[0] * z;
            num = num + p[1];
            num = num * z;
            num = num + p[2];
            num = num * z;
            num = num + p[3];
            num = num * z;
            num = num + p[4];
            float den = z + q[0];
            num = num * z;
            den = den * z;
            den = den + q[1];
            den = den * z;
            den = den + q[2];
            den = den * z;
            den = den + q[3];
            den = den * z;
            den = den + q[4];
            float f = num / den;
            f = f * w;
            r = f + w;
        }
        if (x < 0.0f) {
            float t = table_neg[index];
            float u = t + r;
            return t + u;
        }
        float t = table_pos[index];
        float u = t - r;
        return t + u;
    }

    // func_800BFBE8 with a2 = 1 (the quaternion is taken as unit, scale 2). Note the sign pattern: elements 1, 3, 5,
    // 7 are negated, 2 and 6 are not; this is what 2049 computes and its data is authored for it.
    void quat_to_matrix(float m[9], const float q[4]) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float xs = x * 2.0f;
        float ys = y * 2.0f;
        float zs = z * 2.0f;
        float xx = x * xs;
        float yy = y * ys;
        float zz = z * zs;
        m[0] = 1.0f - (yy + zz);
        m[4] = 1.0f - (xx + zz);
        m[8] = 1.0f - (xx + yy);
        float xy = x * ys;
        float wz = w * zs;
        m[1] = -(xy + wz);
        m[3] = -(xy - wz);
        float yz = y * zs;
        float wx = w * xs;
        m[5] = -(yz + wx);
        m[7] = -(yz - wx);
        float xz = x * zs;
        float wy = w * ys;
        m[2] = xz - wy;
        m[6] = xz + wy;
    }

    // func_800BFD8C: slerp with acos/sin below a 0.98 cosine, a wrapping per-component lerp above it.
    void quat_interp(float t, const float q0[4], const float q1[4], float out[4]) {
        if (t < 0.001f) {
            std::memcpy(out, q0, 16);
            return;
        }
        if (0.999f < t) {
            std::memcpy(out, q1, 16);
            return;
        }
        float yy = q0[1] * q1[1];
        float xx = q0[0] * q1[0];
        float d = xx + yy;
        float zz = q0[2] * q1[2];
        d = d + zz;
        float ww = q1[3] * q0[3];
        d = ww + d;
        float c = d * 0.999f;
        float r[4];
        if (c < 0.0f) {
            r[0] = -q1[0];
            c = -c;
            r[1] = -q1[1];
            r[2] = -q1[2];
            r[3] = -q1[3];
        }
        else {
            std::memcpy(r, q1, 16);
        }
        if (c < 0.98f) {
            float angle = acosf2049(c);
            float inv = 1.0f / sinf2049(angle);
            float a0 = (1.0f - t) * angle;
            float k0 = sinf2049(a0) * inv;
            float a1 = t * angle;
            float k1 = sinf2049(a1) * inv;
            for (int i = 0; i < 4; i++) {
                float u = r[i] * k1;
                float v = q0[i] * k0;
                r[i] = u + v;
            }
        }
        else {
            for (int i = 0; i < 4; i++) {
                float e = r[i] - q0[i];
                if (1.0f < e) {
                    e = e - 2.0f;
                }
                if (e < -1.0f) {
                    e = e + 2.0f;
                }
                float s = t * e;
                float v = q0[i] + s;
                if (1.0f < v) {
                    v = v - 2.0f;
                }
                else if (v < -1.0f) {
                    v = v + 2.0f;
                }
                r[i] = v;
            }
        }
        std::memcpy(out, r, 16);
    }

    // ---- Parsing ----------------------------------------------------------------------------------------------

    bool parse_paths(const uint8_t* data, size_t size, std::vector<Path>& out) {
        out.clear();
        uint32_t base, count;
        if (!find_chunk(data, size, "PTHD", base, count)) {
            return true;
        }
        if ((uint64_t)base + (uint64_t)count * 36 > size) {
            return false;
        }
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t* h = data + base + k * 36;
            Path p;
            p.name.assign((const char*)h, strnlen((const char*)h, 16));
            p.flags = be32(h + 0x10);
            int16_t nodes = be16(h + 0x14);
            p.group = be16(h + 0x16);
            uint32_t link = be32(h + 0x18);
            uint32_t offset = be32(h + 0x1C);
            p.dyn_id = (int32_t)be32(h + 0x20);
            // 2049 relocates +0x18 unconditionally; only a PTHD offset is meaningful (0 = none).
            if (link != 0 && link >= base && (link - base) % 36 == 0 && (link - base) / 36 < count) {
                p.link = (int)((link - base) / 36);
            }
            if (nodes < 0 || (uint64_t)offset + (uint64_t)nodes * 0x44 > size) {
                return false;
            }
            for (int j = 0; j < nodes; j++) {
                const uint8_t* n = data + offset + j * 0x44;
                PathNode node;
                for (int i = 0; i < 3; i++) {
                    node.pos[i] = bef(n + i * 4);
                    node.dir[i] = bef(n + 0xC + i * 4);
                    node.scale[i] = bef(n + 0x18 + i * 4);
                }
                for (int i = 0; i < 4; i++) {
                    node.quat[i] = bef(n + 0x24 + i * 4);
                }
                node.dist = bef(n + 0x34);
                node.time = bef(n + 0x38);
                node.speed = bef(n + 0x3C);
                node.flags = be32(n + 0x40);
                p.nodes.push_back(node);
            }
            out.push_back(std::move(p));
        }
        return true;
    }

    bool parse_types(const uint8_t* main, size_t size, uint32_t main_vram, std::vector<TypeInfo>& out) {
        constexpr uint32_t table = 0x80117530;
        constexpr int rows = 122;
        out.clear();
        if (table < main_vram || table - main_vram + rows * 0x30 > size) {
            return false;
        }
        auto string_at = [&](uint32_t addr, std::string& s) {
            s.clear();
            if (addr < main_vram || addr - main_vram >= size) {
                return addr == 0;
            }
            const char* c = (const char*)main + (addr - main_vram);
            s.assign(c, strnlen(c, size - (addr - main_vram)));
            return true;
        };
        for (int k = 0; k < rows; k++) {
            const uint8_t* r = main + (table - main_vram) + k * 0x30;
            TypeInfo t;
            if (!string_at(be32(r), t.name) || !string_at(be32(r + 4), t.model)) {
                return false;
            }
            t.init = be32(r + 8);
            t.update = be32(r + 0xC);
            t.flags = be32(r + 0x10);
            t.anim = be16(r + 0x14);
            t.kind = r[0x16];
            t.sub = r[0x17];
            t.param = bef(r + 0x18);
            for (int i = 0; i < 3; i++) {
                t.sounds[i] = (int32_t)be32(r + 0x1C + i * 4);
            }
            t.sound_flags = be32(r + 0x28);
            t.sound_range = bef(r + 0x2C);
            out.push_back(std::move(t));
        }
        return true;
    }

    int classify(const std::vector<TypeInfo>& types, const std::string& name) {
        for (size_t k = 0; k < types.size(); k++) {
            const std::string& n = types[k].name;
            if (!n.empty() && name.compare(0, n.size(), n) == 0) {
                return (int)k;
            }
        }
        return -1;
    }

    // ---- Collision groups -------------------------------------------------------------------------------------

    void apply_translate(const float delta[3], int16_t vert[4]) {
        float v[3];
        decode_vert(vert, v);
        int32_t q[3];
        for (int i = 0; i < 3; i++) {
            float n = delta[i] + v[i];
            float s = n * 32.0f;
            q[i] = s < 0.0f ? (int32_t)(s - 0.5f) : (int32_t)(s + 0.5f);
        }
        vert[0] = (int16_t)(q[0] >> 5);
        vert[1] = (int16_t)(q[1] >> 5);
        vert[2] = (int16_t)(q[2] >> 5);
        vert[3] = (int16_t)(uint16_t)((q[2] & 0x1F) | ((q[0] << 10) & 0x7C00) | ((q[1] << 5) & 0x3E0));
    }

    void apply_rotate(const GroupOp& op, const int16_t rest_matrix[9], const int16_t rest_vert[4],
                      int16_t out_matrix[9], int16_t out_vert[4]) {
        // func_800AD650: s16 x 2^-14.
        float p[9];
        for (int i = 0; i < 9; i++) {
            p[i] = (float)rest_matrix[i] * 6.1035156e-05f;
        }
        float a[9], b[9];
        mat_mul(op.inv_rest, p, a); // a = rest x inverse rest pose
        mat_mul(op.m, a, b);        // b = a x object matrix
        for (int i = 0; i < 9; i++) {
            out_matrix[i] = (int16_t)(int32_t)(b[i] * 16384.0f);
        }
        float v[3], d[3], e[3], g[3];
        decode_vert(rest_vert, v);
        for (int i = 0; i < 3; i++) {
            d[i] = v[i] - op.pivot[i];
        }
        vec_mul(d, op.inv_rest, e);
        vec_mul(e, op.m, g);
        int32_t q[3];
        for (int i = 0; i < 3; i++) {
            float n = op.pivot[i] + g[i];
            q[i] = (int32_t)(n * 32.0f);
        }
        out_vert[3] = (int16_t)(uint16_t)((q[2] & 0x1F) + ((q[0] & 0x1F) << 10) + ((q[1] & 0x1F) << 5));
        out_vert[0] = (int16_t)(q[0] >> 5);
        out_vert[1] = (int16_t)(q[1] >> 5);
        out_vert[2] = (int16_t)(q[2] >> 5);
    }

    void rotate_in_place(float m[9], int sub_kind, float dt) {
        if (sub_kind < 3 || sub_kind > 5) {
            return;
        }
        const float* rate = in_place_rates[sub_kind];
        float a[3];
        for (int i = 0; i < 3; i++) {
            a[i] = rate[i] * dt;
        }
        rotate_angles(m, a);
    }

    void rotate_angles(float m[9], const float a[3]) {
        rotate_y(m, a[1]);
        rotate_x(m, a[0]);
        rotate_z(m, a[2]);
    }

    // ---- World ------------------------------------------------------------------------------------------------

    void World::init(std::vector<Path> paths, const std::vector<TypeInfo>& types, const Options& options) {
        paths_ = std::move(paths);
        types_ = types;
        options_ = options;
        objects_.clear();
        order_.clear();
        ops_.clear();
        body_count_ = 0;

        auto group_op = [&](GroupOp::Kind kind, int16_t group) {
            GroupOp op = {};
            op.kind = kind;
            op.group = group;
            ops_.push_back(op);
        };

        // func_800B2DF8: nothing spawns in the restricted mode without the Expansion Pak.
        if (options.restricted && !options.expansion) {
            return;
        }
        for (int k = 0; k < (int)paths_.size(); k++) {
            Path& p = paths_[k];
            if (options.mode == 2 && p.group > 0) {
                bool battle = false;
                for (const PathNode& n : p.nodes) {
                    battle = battle || (n.flags & node_battle);
                }
                if (!battle) {
                    group_op(GroupOp::disable, p.group);
                    continue;
                }
            }
            uint32_t f = p.flags;
            bool present = ((f & path_forward) && (f & path_backward)) || (options.backward && (f & path_backward)) ||
                (!options.backward && (f & path_forward));
            if (!present) {
                if (f & path_switch_group) {
                    group_op(GroupOp::disable, p.group);
                }
                continue;
            }
            // func_800ABCC8, once per spawn node.
            int type = classify(types_, p.name);
            for (PathNode& n : p.nodes) {
                if (!(n.flags & node_spawn) || type < 0) {
                    continue;
                }
                uint32_t low = types_[type].flags & 0xFFFF;
                if (!options.expansion && !(low & 0x20)) {
                    continue;
                }
                if ((low & ~4u) == 0) {
                    continue;
                }
                Object o;
                o.path = k;
                o.type = type;
                o.anim_model = types_[type].anim;
                objects_.push_back(o);
            }
        }

        // func_800AB7D8 + func_800C1604 visit the objects in reverse creation order (the active list is
        // push-front); each init takes the path's first unclaimed spawn node, and pushes the object's instance on
        // the front of the update list, so updates run in creation order.
        for (int i = (int)objects_.size() - 1; i >= 0; i--) {
            Object& o = objects_[i];
            Path& p = paths_[o.path];
            const TypeInfo& t = types_[o.type];
            // func_800ABCC8 sets object flag 8 unless the slot is 6; func_800AB7D8 clears it for PTHD flag 0x10.
            o.slot = (int16_t)(t.flags >> 16);
            o.cars = o.slot != 6 && !(p.flags & path_no_gdat);
            if (o.slot == 4) {
                o.body = true;
                o.body_index = body_count_++;
            }
            o.sound_state = 2;
            o.f.dir = 0;
            o.f.t = 0.0f;
            float max_speed = 0.0f;
            for (const PathNode& n : p.nodes) {
                if (max_speed < n.speed) {
                    max_speed = n.speed;
                }
            }
            o.f.max_speed = max_speed;
            for (int n = 0; n < (int)p.nodes.size(); n++) {
                if ((p.nodes[n].flags & node_spawn) && !(p.nodes[n].flags & node_spawned)) {
                    restart(o, n);
                    p.nodes[n].flags |= node_spawned;
                    break;
                }
            }
            if (t.anim == anim_shark) {
                o.anim_count = 4;
                o.anim_timer = shark_period;
            }
            else if (t.anim == anim_f1flag) {
                o.anim_count = 20;
                o.anim_timer = f1flag_period;
            }
            if ((p.flags & path_wait_trigger) && !(p.flags & path_one_shot)) {
                p.flags = (p.flags & ~path_pad_off) | path_pad_on | path_armed;
                if (!(p.flags & path_no_model)) {
                    o.model = Model::trigger_off;
                }
            }
        }
        for (int i = 0; i < (int)objects_.size(); i++) {
            order_.push_back(i);
        }
    }

    std::vector<GroupOp> World::take_group_ops() {
        std::vector<GroupOp> out;
        out.swap(ops_);
        return out;
    }

    void World::car_on_group(int car, int16_t group, bool on) {
        if (car < 0 || car > 7) {
            return;
        }
        uint32_t bit = 1u << (24 + car);
        for (const Object& o : objects_) {
            Path& p = paths_[o.path];
            if (p.group != group || o.slot != 5 || !o.cars) {
                continue;
            }
            if (on) {
                if ((p.flags >> 24) == 0 || (p.flags & path_one_shot)) {
                    p.flags |= path_triggered;
                }
                p.flags |= bit;
            }
            else {
                p.flags &= ~bit;
            }
        }
    }

    void World::platform_velocity(int16_t group, float out[3]) const {
        out[0] = out[1] = out[2] = 0.0f;
        // 0x8013C300 is filled by the inits, i.e. in reverse creation order.
        for (int i = (int)objects_.size() - 1; i >= 0; i--) {
            const Object& o = objects_[i];
            const Path& p = paths_[o.path];
            if (!(p.flags & path_moves_group) || p.group != group) {
                continue;
            }
            const PathNode& node = p.nodes[o.f.node];
            float s = (o.f.dir & 8) ? -o.f.speed : o.f.speed;
            for (int k = 0; k < 3; k++) {
                out[k] = node.dir[k] * s;
            }
            return;
        }
    }

    void World::fire(int16_t group) {
        for (const Object& o : objects_) {
            Path& p = paths_[o.path];
            if (p.group == group && o.slot == 5 && o.cars) {
                p.flags |= path_triggered;
            }
        }
    }

    // func_800C085C.
    void World::restart(Object& o, int n) {
        Path& p = paths_[o.path];
        Follower& f = o.f;
        int count = (int)p.nodes.size();
        f.dir = ((p.flags & path_pingpong) && n >= count - 1) ? 8 : 4;
        f.node = (int16_t)n;
        f.t = 0.0f;
        f.speed = 0.0f;
        const PathNode& node = p.nodes[n];
        for (int i = 0; i < 3; i++) {
            o.pos[i] = node.pos[i];
            f.prev[i] = node.pos[i];
        }
        uint32_t v = p.flags;
        if (v & (path_wait_trigger | path_stop_at_ends)) {
            v |= path_halted;
            p.flags = v;
            p.follower = (int)(&o - objects_.data());
        }
        if (v & (path_trigger_pad | path_one_shot)) {
            p.flags = v | path_armed;
        }
        quat_to_matrix(o.m, node.quat);
        // func_8008D6B0 + func_800C0828: the inverse rest rotation is the transpose of the unscaled matrix.
        const float* m = o.m;
        float inv[9] = { m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8] };
        std::memcpy(f.inv_rest, inv, sizeof(inv));
        for (int k = 0; k < 9; k++) {
            o.m[k] = o.m[k] * node.scale[k / 3];
        }
    }

    // Sounds: func_800BF394 (start), func_800BF1C8 (stop), func_800BF45C (moving). The pure port reports requests;
    // 2049 also waits for the start sound to finish before the loop (and before a stop), which is left to the host.
    void World::sound_start(Object& o) {
        o.sound = Sound::start;
        o.sound_state = 0;
    }

    void World::sound_stop(Object& o) {
        if (o.sound_state != 2) {
            o.sound = Sound::stop;
            o.sound_state = 2;
        }
    }

    void World::sound_moving(Object& o) {
        if (o.sound_state == 1) {
            return;
        }
        if (o.sound_state == 2) {
            sound_start(o);
            return;
        }
        o.sound = Sound::loop;
        o.sound_state = 1;
    }

    // func_800C04CC.
    void World::position(Object& o, float dt) {
        Path& p = paths_[o.path];
        Follower& f = o.f;
        int count = (int)p.nodes.size();
        int n = f.node;
        if (!(n < count - 1 || (p.flags & path_loop) || !(p.flags & path_stop_at_ends))) {
            return;
        }
        const PathNode& node = p.nodes[n];
        float s;
        if (node.flags & (node_constant_speed | node_constant_speed2)) {
            float tt = (f.dir & 8) ? node.time - f.t : f.t;
            float r = tt / node.time;
            s = node.dist * r;
        }
        else {
            int next = n + 1;
            if (next >= count && (p.flags & path_loop)) {
                next = 0;
            }
            // 2049 reads past the last node here when it isn't looping; that state isn't reachable with its data
            // (the end handling never leaves a non-looping follower on its last node), so clamp.
            next = std::min(next, count - 1);
            float v0 = node.speed;
            float v1 = p.nodes[next].speed;
            float dv = v1 - v0;
            float tt = (f.dir & 8) ? node.time - f.t : f.t;
            float t2 = tt * tt;
            float a = t2 * dv;
            float b = node.time * 2.0f;
            float c = a / b;
            float d = v0 * tt;
            s = d + c;
        }
        float off[3];
        for (int i = 0; i < 3; i++) {
            off[i] = node.dir[i] * s;
        }
        for (int i = 0; i < 3; i++) {
            f.prev[i] = o.pos[i];
        }
        for (int i = 0; i < 3; i++) {
            float q = (float)(int32_t)(off[i] * 32.0f) * 0.03125f;
            o.pos[i] = q + node.pos[i];
        }
        float d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = o.pos[i] - f.prev[i];
        }
        float xx = d[0] * d[0];
        float yy = d[1] * d[1];
        float xy = xx + yy;
        float zz = d[2] * d[2];
        float sum = zz + xy;
        f.speed = std::sqrt(sum) / dt;
        // Mode 5 (the obstacle course) skips this beyond 450 units of car 0 (0x800C0784, 0x80123E88 = 450^2).
        if ((p.flags & path_moves_group) && near_focus(o, 202500.0f)) {
            GroupOp op = {};
            op.kind = GroupOp::translate;
            op.group = p.group;
            std::memcpy(op.delta, d, sizeof(d));
            ops_.push_back(op);
        }
    }

    // func_800C00E0.
    void World::rotation(Object& o) {
        Path& p = paths_[o.path];
        Follower& f = o.f;
        int count = (int)p.nodes.size();
        int n = f.node;
        const PathNode& node = p.nodes[n];
        if (node.flags & node_rotation_fixed) {
            quat_to_matrix(o.m, node.quat);
        }
        else {
            float tt = (f.dir & 8) ? node.time - f.t : f.t;
            if (node.time == 0.0f) {
                quat_to_matrix(o.m, node.quat);
            }
            else {
                int next = n + 1;
                if (next >= count && (p.flags & path_loop)) {
                    next = 0;
                }
                next = std::min(next, count - 1); // see position()
                float q[4];
                quat_interp(tt / node.time, node.quat, p.nodes[next].quat, q);
                quat_to_matrix(o.m, q);
            }
        }
        // Mode 5 (the obstacle course) skips this beyond 150 units of car 0 (0x800C01E8, 0x80123E84 = 150^2).
        if ((p.flags & path_moves_group) && near_focus(o, 22500.0f)) {
            GroupOp op = {};
            op.kind = GroupOp::rotate;
            op.group = p.group;
            std::memcpy(op.pivot, o.pos, sizeof(op.pivot));
            std::memcpy(op.inv_rest, f.inv_rest, sizeof(op.inv_rest));
            std::memcpy(op.m, o.m, sizeof(op.m));
            ops_.push_back(op);
        }
    }

    bool World::near_focus(const Object& o, float range2) const {
        if (options_.mode != 5) {
            return true;
        }
        float d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = o.pos[i] - focus_[i];
        }
        float xy = d[0] * d[0] + d[1] * d[1];
        float sum = d[2] * d[2] + xy;
        return !(range2 < sum);
    }

    void World::set_focus(const float pos[3]) {
        std::memcpy(focus_, pos, sizeof(focus_));
    }

    void World::update(float dt) {
        for (int i : order_) {
            step(objects_[i], dt);
        }
    }

    // func_800C0AC0.
    void World::step(Object& o, float dt) {
        Path& p = paths_[o.path];
        Follower& f = o.f;
        const TypeInfo& t = types_[o.type];
        auto restore = [&]() {
            GroupOp op = {};
            op.kind = GroupOp::restore;
            op.group = p.group;
            ops_.push_back(op);
        };
        auto show = [&](Model m) {
            if (!(p.flags & path_no_model)) {
                o.model = m;
            }
        };
        o.sound = Sound::none;

        // Trigger state machine.
        uint32_t a = p.flags;
        if (a & path_wait_trigger) {
            if (a & path_one_shot) {
                // Runs forward while triggered (0x200 is re-set every frame a car is on it); otherwise it turns
                // back and runs to its start, where func_800C085C halts and re-arms it.
                if (!(a & path_triggered)) {
                    if (!(a & path_armed)) {
                        if (f.dir & 4) {
                            f.dir = (uint16_t)((f.dir & ~4) | 8);
                            f.t = p.nodes[f.node].time - f.t;
                        }
                        if (p.flags & path_halted) {
                            sound_start(o);
                        }
                        p.flags &= ~path_halted;
                    }
                }
                else {
                    if (a & path_armed) {
                        p.flags = a & ~(path_halted | path_armed);
                        sound_start(o);
                    }
                    p.flags &= ~path_triggered;
                }
            }
            else {
                Path* link = p.link >= 0 ? &paths_[p.link] : nullptr;
                if ((a & path_linked_pad) && (a & path_pad_off) && link && (link->flags & path_halted)) {
                    // The linked object stopped: re-arm the pad.
                    p.flags = (a & ~path_pad_off) | path_pad_on | path_armed;
                    show(Model::trigger_off);
                    sound_stop(o);
                    return;
                }
                if ((a & path_armed) && (a & path_triggered) && link) {
                    if (link->flags & path_halted) {
                        link->flags &= ~path_halted;
                    }
                    else if ((a & path_trigger_pad) && link->follower >= 0) {
                        Follower& lf = objects_[link->follower].f;
                        lf.dir = (lf.dir & 8) ? (uint16_t)((lf.dir & ~8) | 4) : (uint16_t)((lf.dir & ~4) | 8);
                        lf.t = link->nodes[lf.node].time - lf.t;
                    }
                    a = p.flags & ~(path_halted | path_triggered | path_armed);
                    p.flags = a;
                    if (a & path_trigger_pad) {
                        if (a & path_pad_on) {
                            p.flags = (a & ~path_pad_on) | path_pad_off;
                            show(Model::trigger_on);
                            sound_start(o);
                        }
                        else {
                            p.flags = (a & ~path_pad_off) | path_pad_on;
                            show(Model::trigger_off);
                            sound_stop(o);
                        }
                    }
                    else if (a & path_pad_on) {
                        p.flags = (a & ~path_pad_on) | path_pad_off;
                        show(Model::trigger_on);
                        sound_start(o);
                    }
                }
            }
        }
        if (p.flags & path_halted) {
            return;
        }

        // Advance along the path.
        int count = (int)p.nodes.size();
        f.t = f.t + dt;
        bool done = false;
        do {
            const PathNode& node = p.nodes[f.node];
            float time = node.time;
            if (!(time <= f.t)) {
                break;
            }
            f.t = f.t - time;
            if (f.dir & 8) {
                if (f.node == 0) {
                    if (p.flags & path_one_shot) {
                        done = true;
                        restart(o, 0);
                        sound_stop(o);
                    }
                    else {
                        f.dir = (uint16_t)((f.dir & ~8) | 4);
                        if (p.flags & path_stop_at_ends) {
                            p.flags |= path_halted;
                            done = true;
                            f.t = 0.0f;
                        }
                    }
                    if (p.flags & path_moves_group) {
                        restore();
                    }
                }
                else {
                    f.node--;
                }
            }
            else if (count == f.node + 1) {
                // On the last node (a looping path's closing segment, or a stop-at-ends path's end).
                if (p.flags & path_stop_at_ends) {
                    f.dir = (uint16_t)((f.dir & ~4) | 8);
                    f.node--;
                }
                else {
                    f.node = 0;
                    if (p.flags & path_moves_group) {
                        restore();
                    }
                }
            }
            else {
                f.node++;
                int n = f.node;
                if (p.nodes[n].flags & node_halt) {
                    done = true;
                    p.flags |= path_halted;
                    f.t = 0.0f;
                }
                else if (count == n + 1) {
                    uint32_t v = p.flags;
                    if (v & path_pingpong) {
                        f.dir = (uint16_t)((f.dir & ~4) | 8);
                        f.node = (int16_t)(n - 1);
                    }
                    else if (v & path_one_shot) {
                        f.dir = (uint16_t)((f.dir & ~4) | 8);
                        f.node = (int16_t)(n - 1);
                        f.t = 0.0f;
                        p.flags |= path_halted;
                    }
                    else if (!(v & path_loop)) {
                        restart(o, 0);
                        if (p.flags & path_moves_group) {
                            restore();
                        }
                    }
                }
            }
        } while (!done);

        // Pose. Position and rotation run unless the node turns them off; scale is applied below.
        bool moved = false;
        uint32_t nf = p.nodes[f.node].flags;
        if (!(nf & node_no_translation)) {
            position(o, dt);
            moved = true;
            nf = p.nodes[f.node].flags;
        }
        if (!(nf & node_no_scale) || !(nf & node_no_rotation_a)) {
            moved = true;
            rotation(o);
        }
        if (moved) {
            if (p.flags & path_halted) {
                sound_stop(o);
            }
            else {
                sound_moving(o);
            }
        }
        else {
            sound_stop(o);
        }

        // Flip-book. Until the frame timer runs out 2049 returns here, skipping the body update and the scale.
        if (t.anim != -1) {
            o.anim_timer = o.anim_timer - dt;
            if (!(o.anim_timer <= 0.0f)) {
                return;
            }
            if (t.anim == anim_shark) {
                o.anim_timer = shark_period;
            }
            else if (t.anim == anim_f1flag) {
                o.anim_timer = f1flag_period;
            }
            o.anim_frame++;
            if (o.anim_frame >= o.anim_count) {
                o.anim_frame = 0;
            }
            o.anim_model = t.anim + o.anim_frame;
        }

        const PathNode& node = p.nodes[f.node];
        // func_800AB750: the body table gets the matrix, position and node direction x speed.
        if (o.body) {
            if (f.dir & 8) {
                float s = -f.speed;
                for (int i = 0; i < 3; i++) {
                    o.body_vel[i] = node.dir[i] * s;
                }
            }
            else {
                for (int i = 0; i < 3; i++) {
                    o.body_vel[i] = node.dir[i] * f.speed;
                }
            }
        }

        // Scale: lerp to the next node (wrapping to 0 whatever the path flags) and multiply the rows. When the
        // rotation wasn't recomputed this frame the scale compounds; 2049's data only scales nodes that rotate.
        if (!(node.flags & node_no_scale)) {
            int next = f.node + 1;
            if (next >= count) {
                next = 0;
            }
            float tt = (f.dir & 8) ? node.time - f.t : f.t;
            float r = tt / node.time;
            float s[3];
            for (int i = 0; i < 3; i++) {
                float s0 = node.scale[i];
                float d = p.nodes[next].scale[i] - s0;
                float e = d * r;
                s[i] = e + s0;
            }
            for (int k = 0; k < 9; k++) {
                o.m[k] = o.m[k] * s[k / 3];
            }
        }
    }
}
