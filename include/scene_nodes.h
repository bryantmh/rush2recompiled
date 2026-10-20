#ifndef __SCENE_NODES_H__
#define __SCENE_NODES_H__

#include <cstdint>

// Rush 2's scene node table: 0x38 bytes per node (+0 flags, +4 -> {matrix[9], position[3]}), appended by
// scene_node_create_8007F6DC with no bound, count at 0x800FAE58. The game kept it at 0x800D9E90, with room for 1,000
// nodes before the globals at 0x800E7950; the arcade Haight made 1,045 and ran over text_state_stack_a (0x800E7C50),
// so us.toml points every game access at a copy at 0x80400000 (tools/players4/relocate.py, PLACED) with room for
// 2,340, below the game heap (which starts at 0x80420000, src/assets.cpp).
namespace rush2::scene_nodes {
    constexpr uint32_t table = 0x80400000;
    constexpr uint32_t table_end = 0x80420000;
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t capacity = (table_end - table) / node_size;
    constexpr uint32_t count = 0x800FAE58;   // s32: nodes in use
}

#endif
