#ifndef __TRACK2049_MOVERS_LOGIC_H__
#define __TRACK2049_MOVERS_LOGIC_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Rush 2049's animated track objects (path followers), ported as pure logic: no RDRAM, no recomp context. The
// specification, with every address, is docs/rush2049_research/movers.md; src/track2049_movers_logic.cpp has the
// per-function notes. Matrices are 3x3 row-major float[9] as 2049 stores them (row r = m[3r..3r+2], a point is
// transformed as a row vector, p' = p * m). Units are 2049 world units (feet), the same as Rush 2's.
namespace rush2::track2049::movers {
    // ---- Data -------------------------------------------------------------------------------------------------

    // PATH node, 0x44 bytes.
    struct PathNode {
        float pos[3];
        float dir[3];       // unit vector towards the next node
        float scale[3];
        float quat[4];      // x, y, z, w
        float dist;         // segment length
        float time;         // segment duration (s)
        float speed;        // speed at this node (units/s)
        uint32_t flags;     // NodeFlags
    };

    // PTHD header, 36 bytes, plus its nodes.
    struct Path {
        std::string name;   // type name (prefix-matched against the type table)
        uint32_t flags = 0; // PathFlags; the runtime bits are rewritten while the race runs, as in 2049
        int16_t group = 0;  // collision group / trigger id (PTHD +0x16)
        int link = -1;      // PTHD +0x18 (a file offset 2049 relocates to a PTHD pointer) as a path index, or -1
        int32_t dyn_id = -1;
        std::vector<PathNode> nodes;
        int follower = -1;  // PTHD +0x20 at runtime: the object restarted last on a 0xC0 path (index into objects)
    };

    enum PathFlags : uint32_t {
        path_pingpong = 0x1,
        path_loop = 0x2,
        path_forward = 0x4,
        path_backward = 0x8,
        path_no_gdat = 0x10,        // clears object flag 8 (spatial-index / car callback) [I]
        path_moves_group = 0x20,    // the collision group `group` moves with the object
        path_wait_trigger = 0x40,
        path_stop_at_ends = 0x80,
        path_halted = 0x100,        // runtime
        path_triggered = 0x200,     // runtime: set by a car on the group's polygons
        path_armed = 0x400,         // runtime
        path_switch_group = 0x800,  // group disabled when the path is absent in this direction
        path_trigger_pad = 0x1000,  // TRIGGER pad: starts or reverses its link
        path_linked_pad = 0x2000,   // TRIGGER pad: re-arms when its link halts
        path_one_shot = 0x4000,     // runs while triggered, then returns
        path_no_model = 0x8000,     // no instance model writes (TRIGGERON/OFF, flip-book) and no sounds
        path_pad_off = 0x100000,    // runtime: pad shows TRIGGERON (fired)
        path_pad_on = 0x200000,     // runtime: pad shows TRIGGEROFF (armed)
        path_cars_on = 0xFF000000,  // runtime: bit 24 + car = car i has a wheel on the group's polygons
    };

    enum NodeFlags : uint32_t {
        node_spawn = 0x1,
        node_no_translation = 0x2,
        node_no_rotation_a = 0x4,   // 0x4 and 0x10 together: no rotation update
        node_constant_speed = 0x8,  // also 0x10000000
        node_no_scale = 0x10,
        node_rotation_fixed = 0x20, // matrix = this node's quaternion, no interpolation
        node_halt = 0x40,
        node_spawned = 0x1000,      // runtime
        node_battle = 0x1000000,
        node_constant_speed2 = 0x10000000,
    };

    // Row of 2049's dynamic-object type table (main data 0x80117530, 122 x 0x30).
    struct TypeInfo {
        std::string name;
        std::string model;
        uint32_t init = 0, update = 0;
        uint32_t flags = 0;     // flags >> 16 == 4: moving body (body table entry, kind-4 velocity)
        int16_t anim = -1;      // flip-book base model index (361 SHARK: 4 frames, 365 F1FLAG: 20 frames)
        uint8_t kind = 0, sub = 0;
        float param = 0.0f;     // body box class for moving bodies [I]
        int32_t sounds[3] = { -1, -1, -1 }; // start, loop, stop
        uint32_t sound_flags = 0;
        float sound_range = 0.0f;
    };

    // Parses PTHD/PATH from a big-endian 2049 track geometry file (files 101-119). Returns false on bad data; a file
    // without PTHD gives an empty list.
    bool parse_paths(const uint8_t* data, size_t size, std::vector<Path>& out);

    // Parses the type table from 2049's decompressed main data (loaded at main_vram = 0x80086A50).
    bool parse_types(const uint8_t* main, size_t size, uint32_t main_vram, std::vector<TypeInfo>& out);

    // func_800ABCC8's lookup: the first row whose name is a prefix of `name`, or -1.
    int classify(const std::vector<TypeInfo>& types, const std::string& name);

    // ---- Runtime ----------------------------------------------------------------------------------------------

    // Path follower state (2049 pool 0x80138880, object +0x6C).
    struct Follower {
        float t = 0.0f;         // +0x04 time into the current segment
        int16_t node = 0;       // +0x0C
        uint16_t dir = 4;       // +0x0E 4 forward, 8 backward
        float speed = 0.0f;     // +0x10 |position change| / dt
        float max_speed = 0.0f; // +0x14 largest node speed
        float prev[3] = {};     // +0x18 position before the last step
        float inv_rest[9] = {}; // +0x24 transpose of the restart node's rotation (unscaled)
    };

    enum class Model : uint8_t {
        normal,         // the type's model
        trigger_off,    // TRIGGEROFF (2049 handle 0x80142A7A): pad armed
        trigger_on,     // TRIGGERON (0x80142A78): pad fired
    };

    enum class Sound : uint8_t { none, start, loop, stop };

    struct Object {
        int path = -1;
        int type = -1;
        Follower f;
        float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // object +0x14: rotation x scale (row r scaled by scale[r])
        float pos[3] = {};                          // object +0x38
        int slot = 0;               // type flags >> 16: car callback slot (4 moving body, 5 trigger test)
        bool cars = true;           // object flag 8: func_800BEAA0 runs the slot callback against nearby cars;
                                    // cleared by PTHD flag 0x10 (and for slot 6)
        bool body = false;          // slot 4: has a moving-body entry (tested against cars only if `cars`)
        int body_index = -1;        // object +0x65 for bodies
        float body_vel[3] = {};     // body table +0x34: node dir x speed (negated going backward)
        Model model = Model::normal;
        int anim_frame = 0;         // flip-book: instance +0x04
        float anim_timer = 0.0f;    // flip-book: instance +0x10
        int anim_count = 0;         // object +0x5A
        int anim_model = -1;        // object +0x50: 2049 model index shown (anim base + frame)
        int sound_state = 2;        // object +0x64: 0 started, 1 looping, 2 stopped
        Sound sound = Sound::none;  // last sound request this update
    };

    // Collision-group operations, in the order 2049 performs them (func_800C0294, func_800BF838, func_800B2D20,
    // func_800B2CB4). apply_* below reproduce their arithmetic on raw POLY/VERT data.
    struct GroupOp {
        enum Kind : uint8_t { translate, rotate, restore, disable } kind;
        int16_t group;
        float delta[3];     // translate: position change of the object this step
        float pivot[3];     // rotate: object position
        float inv_rest[9];  // rotate: follower inverse rest rotation
        float m[9];         // rotate: object matrix (rotation x scale)
    };

    struct Options {
        bool backward = false;      // 2049 0x80152570 (Rush 2 0x80119848)
        bool expansion = true;      // 2049 0x80156994 (osMemSize > 4 MB): spawn everything
        bool restricted = false;    // 2049 0x801174B4 & 8 [I: multiplayer]: without expansion nothing spawns
        int mode = 0;               // 2049 0x8014A110; 2 = battle-only nodes [I]; race tracks use neither 2 nor 5
    };

    class World {
    public:
        // func_800B338C/func_800B2DF8 + func_800C1604: spawns the objects present for these options and records
        // the initial group operations (disable for absent 0x800 paths).
        void init(std::vector<Path> paths, const std::vector<TypeInfo>& types, const Options& options);

        // func_800C0AC0 for every object in 2049's instance-list order, once per rendered frame with the frame time.
        void update(float dt);

        // Code at 0x8010C2E4 (callback slot 5: TRIGGER, T3POD, T3TEETER), per object with `cars` set and car:
        // `on` = one of the car's four wheel surface polygons (last found, even airborne) has info bit 0x20 and
        // info >> 11 == the path's group. 2049 skips wrecked cars (and player +0x6C4 >= 0): don't report them. 2049
        // only tests objects listed in the car's GDAT leaf; the host may test all of them.
        void car_on_group(int car, int16_t group, bool on);

        // Fires every trigger-tested path of a group as if a new car had driven onto its polygons (edge trigger
        // without a car bit).
        void fire(int16_t group);

        // func_800C1A00: velocity of a ride-on platform group (first PTHD 0x20 object with this group, in spawn
        // order): its node direction x follower speed, negated going backward; zero if none.
        void platform_velocity(int16_t group, float out[3]) const;

        const std::vector<Object>& objects() const { return objects_; }
        const std::vector<Path>& paths() const { return paths_; }
        const std::vector<TypeInfo>& types() const { return types_; }

        // Group operations since the last take_group_ops() (init's are included in the first call).
        std::vector<GroupOp> take_group_ops();

    private:
        void restart(Object& o, int node);
        void step(Object& o, float dt);
        void position(Object& o, float dt);
        void rotation(Object& o);
        void sound_start(Object& o);
        void sound_stop(Object& o);
        void sound_moving(Object& o);

        std::vector<Path> paths_;
        std::vector<TypeInfo> types_;
        std::vector<Object> objects_;
        std::vector<int> order_;    // update order
        std::vector<GroupOp> ops_;
        Options options_;
        int body_count_ = 0;
    };

    // func_800C0294: moves a group vertex (8-byte VERT: s16 x, y, z, u16 fractions) by delta, rounding half away
    // from zero to 1/32.
    void apply_translate(const float delta[3], int16_t vert[4]);

    // func_800BF838: rewrites a group polygon's matrix (s16[9], x16384) and origin vertex from the MOVER record's
    // rest copies (rest_matrix = MOVER +4, rest_vert = MOVER +0x18).
    void apply_rotate(const GroupOp& op, const int16_t rest_matrix[9], const int16_t rest_vert[4],
                      int16_t out_matrix[9], int16_t out_vert[4]);

    // In-place animations (func_8010E694: TROLLEY2 sub-kind 3, WINDMILL 4, WINDMILL2 5): rotates a placement
    // object's matrix by its sub-kind's rate x dt (func_800D03DC). Other sub-kinds are left unchanged.
    void rotate_in_place(float m[9], int sub_kind, float dt);

    // 2049's math library, bit-exact (libultra sinf/cosf, func_8009C3F8 acosf, func_800BFBE8, func_800BFD8C).
    float sinf2049(float x);
    float cosf2049(float x);
    float acosf2049(float x);
    void quat_to_matrix(float m[9], const float q[4]);
    void quat_interp(float t, const float q0[4], const float q1[4], float out[4]);
}

#endif
