#ifndef __RUSH2_HOOKS_H__
#define __RUSH2_HOOKS_H__

// Declarations for runtime functions called from hooks inserted into recompiled code (see us.toml).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);

// Lets other game threads run (and processes pending RCP/VI events) for up to 1ms.
void yield_self_1ms(uint8_t* rdram);

#ifdef __cplusplus
}
#endif

#endif
