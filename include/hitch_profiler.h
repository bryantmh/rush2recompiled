#pragma once

// Hitch profiler (test aid, docs/hitch_profiler.md): with RUSH2_HITCH_LOG=<file> set, samples the call stack of every
// thread in the process about once a millisecond and logs them with frame markers, so tools/hitch_profile.py can name the
// code that ran during each long frame. Does nothing unless the variable is set.
namespace rush2::hitch {
    // Starts the sampler if RUSH2_HITCH_LOG is set. Call once, as early in main as possible.
    void start();
    // Logs a named marker at the current time (shows in the report next to the hitches it falls in).
    void mark(const char* tag);
}
