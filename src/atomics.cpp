// Replacements for Rush 2 functions that N64Recomp can't translate or that touch hardware directly.

#include <mutex>

#include "recomp.h"

static std::mutex atomic_mutex;

// u32 func_80003B34(u32* addr, u32 bits): atomically sets `bits` in *addr, returns (old & bits).
extern "C" void atomic_set_bits_80003B34(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ atomic_mutex };
    uint32_t old = MEM_W(0, ctx->r4);
    uint32_t bits = (uint32_t)ctx->r5;
    MEM_W(0, ctx->r4) = old | bits;
    ctx->r2 = (gpr)(int32_t)(old & bits);
}

// u32 func_80003B54(u32* addr, u32 bits): atomically clears `bits` in *addr, returns (old & bits).
extern "C" void atomic_clear_bits_80003B54(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ atomic_mutex };
    uint32_t old = MEM_W(0, ctx->r4);
    uint32_t bits = (uint32_t)ctx->r5;
    MEM_W(0, ctx->r4) = old & ~bits;
    ctx->r2 = (gpr)(int32_t)(old & bits);
}

// s32 __osGetId(OSPfs* pfs), called directly by the game's controller pak thread (func_80098D14) to retry
// a pak whose ID area is unreadable. The emulated Controller Pak (src/pak.cpp) never reports an ID error,
// so this is unreachable; report no pack.
extern "C" void os_get_id_8000D090(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 1; // PFS_ERR_NOPACK
}
