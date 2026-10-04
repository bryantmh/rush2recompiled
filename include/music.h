#ifndef __MUSIC_H__
#define __MUSIC_H__

#include <cstdint>

#include "recomp.h"

// Race music across Rush 2, SF Rush and Rush 2049 (src/music.cpp): which songs the races may play, how a race picks
// one, and song previews on the Sound tab.
namespace rush2::music {
    // The Sound tab (replaces the frontend's): main volume, Race Music and the song list. Call instead of
    // recompui::config::create_sound_tab(), before finalize().
    void create_sound_tab();
    // Applies the loaded settings. Call after finalize().
    void load_config();
}

namespace rush2::track1 {
    // Rush 2's sequence number for SF Rush song `song` (0-15), installing SF Rush's songs and bank first (src/
    // track1_audio.cpp), or -1 if they aren't available. Call from a game thread.
    int song_sequence(uint8_t* rdram, recomp_context* ctx, int song);
}

#endif
