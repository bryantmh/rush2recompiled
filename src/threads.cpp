// osStartThread/osStopThread wrappers with libultra semantics.
//
// Rush 2 uses osStopThread(other) ... osStartThread(other) to keep another thread from running while it
// edits shared state (e.g. the pending graphics task counter in func_800B4214). In libultra, starting a
// thread that isn't stopped does nothing. The runtime's osStartThread always schedules the thread, so a
// thread blocked on a message queue ended up in the run queue too, corrupting the scheduler and
// deadlocking the game (all threads waiting to be resumed).
//
// Game threads are cooperative under the runtime, so another thread can't run while the current one is
// inside such a critical section: stopping another thread can be a no-op, and starting a thread only
// needs to do anything for threads that were never started.

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

extern "C" void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx);

extern "C" void rush2_osStartThread(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    OSThread* t = TO_PTR(OSThread, t_);
    if (t->state != STOPPED) {
        return;
    }
    osStartThread_recomp(rdram, ctx);
}

extern "C" void rush2_osStopThread(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    if (t_ == NULLPTR || t_ == ultramodern::this_thread()) {
        osStopThread_recomp(rdram, ctx);
    }
}
